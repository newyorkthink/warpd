#if defined(HAVE_OPENCV)

/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * Common OpenCV detector - Shared detection logic
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "../platform.h"

extern const char *config_get(const char *key);
extern int config_get_int(const char *key);

#ifdef __cplusplus
}
#endif

#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace {

struct Candidate {
    cv::Rect rect;
    double score;
    bool strong;
};

static double intersection_over_union(const cv::Rect &a, const cv::Rect &b)
{
    const cv::Rect intersection = a & b;
    if (intersection.area() <= 0)
        return 0.0;

    const double union_area =
        static_cast<double>(a.area()) + b.area() - intersection.area();
    return union_area > 0.0 ? intersection.area() / union_area : 0.0;
}

static double containment_ratio(const cv::Rect &a, const cv::Rect &b)
{
    const cv::Rect intersection = a & b;
    const int smaller_area = std::min(a.area(), b.area());
    if (intersection.area() <= 0 || smaller_area <= 0)
        return 0.0;

    return static_cast<double>(intersection.area()) / smaller_area;
}

static double border_edge_ratio(const cv::Mat &edges, const cv::Rect &rect)
{
    const cv::Rect bounds(0, 0, edges.cols, edges.rows);
    const cv::Rect clipped = rect & bounds;
    if (clipped.width < 4 || clipped.height < 4)
        return 0.0;

    const cv::Mat roi = edges(clipped);
    const int band = std::max(1, std::min(3, std::min(clipped.width, clipped.height) / 4));

    int edge_pixels = 0;
    int border_pixels = 0;

    edge_pixels += cv::countNonZero(roi.rowRange(0, band));
    border_pixels += band * clipped.width;

    edge_pixels += cv::countNonZero(roi.rowRange(clipped.height - band, clipped.height));
    border_pixels += band * clipped.width;

    if (clipped.height > band * 2) {
        const cv::Mat middle = roi.rowRange(band, clipped.height - band);
        edge_pixels += cv::countNonZero(middle.colRange(0, band));
        edge_pixels += cv::countNonZero(
            middle.colRange(clipped.width - band, clipped.width));
        border_pixels += 2 * band * middle.rows;
    }

    return border_pixels > 0
               ? static_cast<double>(edge_pixels) / border_pixels
               : 0.0;
}

static double edge_density(const cv::Mat &edges, const cv::Rect &rect)
{
    const cv::Rect bounds(0, 0, edges.cols, edges.rows);
    const cv::Rect clipped = rect & bounds;
    if (clipped.area() <= 0)
        return 0.0;

    return static_cast<double>(cv::countNonZero(edges(clipped))) /
           clipped.area();
}

static bool reading_order(const cv::Rect &a, const cv::Rect &b)
{
    const int row_tolerance = std::max(12, std::min(a.height, b.height) / 2);
    if (std::abs(a.y - b.y) <= row_tolerance)
        return a.x < b.x;
    return a.y < b.y;
}

static std::vector<cv::Rect> select_best_candidates(
    std::vector<Candidate> candidates,
    int limit)
{
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate &a, const Candidate &b) {
            if (std::abs(a.score - b.score) > 0.01)
                return a.score > b.score;
            return a.rect.area() < b.rect.area();
        });

    const size_t strong_count = static_cast<size_t>(std::count_if(
        candidates.begin(), candidates.end(),
        [](const Candidate &candidate) { return candidate.strong; }));

    std::vector<cv::Rect> selected;
    for (const Candidate &candidate : candidates) {
        /* When enough clear controls exist, discard weak text-like fallbacks. */
        if (strong_count >= 10 && candidate.score < 40.0)
            continue;

        bool duplicate = false;
        for (const cv::Rect &existing : selected) {
            const double iou = intersection_over_union(candidate.rect, existing);
            const double contained = containment_ratio(candidate.rect, existing);

            if (iou >= 0.48 || contained >= 0.90) {
                duplicate = true;
                break;
            }

            const cv::Point candidate_center(
                candidate.rect.x + candidate.rect.width / 2,
                candidate.rect.y + candidate.rect.height / 2);
            const cv::Point existing_center(
                existing.x + existing.width / 2,
                existing.y + existing.height / 2);

            if (cv::norm(candidate_center - existing_center) < 10.0) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
            selected.push_back(candidate.rect);

        if (static_cast<int>(selected.size()) >= limit)
            break;
    }

    std::sort(selected.begin(), selected.end(), reading_order);
    return selected;
}

} // namespace

/**
 * Remove near-duplicate rectangles without deleting a useful control merely
 * because it overlaps a much larger page/container rectangle.
 */
std::vector<cv::Rect> deduplicate_rectangles(std::vector<cv::Rect> &rects)
{
    if (rects.size() <= 1)
        return rects;

    std::sort(
        rects.begin(), rects.end(),
        [](const cv::Rect &a, const cv::Rect &b) {
            return a.area() < b.area();
        });

    std::vector<cv::Rect> result;
    for (const cv::Rect &rect : rects) {
        bool duplicate = false;

        for (const cv::Rect &existing : result) {
            const double iou = intersection_over_union(rect, existing);
            const double contained = containment_ratio(rect, existing);

            if (iou >= 0.62 || contained >= 0.94) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
            result.push_back(rect);
    }

    std::sort(result.begin(), result.end(), reading_order);
    return result;
}

/**
 * Detect interactive-looking rectangular UI elements.
 *
 * OpenCV cannot know whether text is actually clickable, so this detector
 * deliberately favors visible controls with borders/compact geometry and
 * rejects ordinary body text. AT-SPI remains the preferred detector.
 */
std::vector<cv::Rect> detect_rectangles(const cv::Mat &img)
{
    cv::Mat gray;
    cv::Mat blurred;
    cv::Mat edges;
    cv::Mat connected_edges;

    const int configured_min_area = config_get_int("opencv_min_area");
    const int max_area = config_get_int("opencv_max_area");
    const int configured_min_width = config_get_int("opencv_min_width");
    const int configured_min_height = config_get_int("opencv_min_height");
    const int max_width = config_get_int("opencv_max_width");
    const int max_height = config_get_int("opencv_max_height");
    const double min_aspect = std::atof(config_get("opencv_min_aspect"));
    const double max_aspect = std::atof(config_get("opencv_max_aspect"));

    /* Prevent permissive legacy defaults from turning every glyph into a hint. */
    const int min_area = std::max(configured_min_area, 220);
    const int min_width = std::max(configured_min_width, 14);
    const int min_height = std::max(configured_min_height, 14);
    const int max_elements = std::min(config_get_int("ui_max_elements"), 48);

    fprintf(
        stderr,
        "OpenCV: Effective filters - area: %d-%d, size: %dx%d to %dx%d, "
        "aspect: %.2f-%.2f, maximum: %d\n",
        min_area, max_area, min_width, min_height, max_width, max_height,
        min_aspect, max_aspect, max_elements);

    if (img.channels() == 4)
        cv::cvtColor(img, gray, cv::COLOR_BGRA2GRAY);
    else if (img.channels() == 3)
        cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    else
        gray = img.clone();

    cv::GaussianBlur(gray, blurred, cv::Size(3, 3), 0);
    cv::Canny(blurred, edges, 60, 170);

    /* Join small gaps in rounded control borders without heavily merging text. */
    cv::morphologyEx(
        edges,
        connected_edges,
        cv::MORPH_CLOSE,
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)),
        cv::Point(-1, -1),
        1);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(
        connected_edges,
        contours,
        cv::RETR_LIST,
        cv::CHAIN_APPROX_SIMPLE);

    std::vector<Candidate> candidates;
    int rejected_geometry = 0;
    int rejected_text = 0;
    int rejected_score = 0;

    for (const std::vector<cv::Point> &contour : contours) {
        const cv::Rect rect = cv::boundingRect(contour);
        const int bounding_area = rect.area();

        if (bounding_area < min_area || bounding_area > max_area ||
            rect.width < min_width || rect.width > max_width ||
            rect.height < min_height || rect.height > max_height) {
            rejected_geometry++;
            continue;
        }

        const double aspect = static_cast<double>(rect.width) / rect.height;
        if (aspect < min_aspect || aspect > max_aspect) {
            rejected_geometry++;
            continue;
        }

        const double contour_area = std::abs(cv::contourArea(contour));
        const double fill_ratio = bounding_area > 0
                                      ? contour_area / bounding_area
                                      : 0.0;
        const double border_ratio = border_edge_ratio(edges, rect);
        const double density = edge_density(edges, rect);
        const double perimeter = cv::arcLength(contour, true);
        const double compactness = perimeter > 0.0
                                       ? 4.0 * CV_PI * contour_area /
                                             (perimeter * perimeter)
                                       : 0.0;

        std::vector<cv::Point> polygon;
        cv::approxPolyDP(contour, polygon, perimeter * 0.025, true);
        const bool polygonal_box =
            polygon.size() >= 4 && polygon.size() <= 10 &&
            cv::isContourConvex(polygon) && fill_ratio >= 0.42;

        const bool icon_like =
            rect.width >= 20 && rect.height >= 20 &&
            rect.width <= 96 && rect.height <= 96 &&
            aspect >= 0.55 && aspect <= 1.80 &&
            compactness >= 0.08 && density >= 0.035;

        const bool input_like =
            rect.width >= 50 && rect.height >= 20 && rect.height <= 90 &&
            aspect >= 1.40 && (border_ratio >= 0.11 || polygonal_box);

        const bool button_like =
            rect.width >= 24 && rect.width <= 360 &&
            rect.height >= 18 && rect.height <= 96 &&
            (border_ratio >= 0.14 || (polygonal_box && fill_ratio >= 0.50));

        const bool text_like =
            rect.height <= 32 && aspect >= 1.8 &&
            border_ratio < 0.10 && fill_ratio < 0.48;

        const bool page_container =
            rect.width >= img.cols * 0.72 || rect.height >= img.rows * 0.45;

        if (text_like && !input_like && !button_like) {
            rejected_text++;
            continue;
        }

        double score = 0.0;
        score += std::min(border_ratio * 100.0, 34.0);
        score += std::min(fill_ratio * 24.0, 24.0);
        score += std::min(compactness * 28.0, 12.0);

        if (polygonal_box)
            score += 20.0;
        if (input_like)
            score += 18.0;
        if (button_like)
            score += 16.0;
        if (icon_like)
            score += 12.0;
        if (page_container)
            score -= 35.0;
        if (density > 0.55)
            score -= 12.0; /* Usually dense text or a textured image region. */

        if (score < 28.0 ||
            (!polygonal_box && !input_like && !button_like && !icon_like)) {
            rejected_score++;
            continue;
        }

        candidates.push_back({rect, score, score >= 45.0});
    }

    fprintf(
        stderr,
        "OpenCV: candidates=%zu, rejected geometry=%d, text=%d, score=%d\n",
        candidates.size(), rejected_geometry, rejected_text, rejected_score);

    return select_best_candidates(candidates, std::max(1, max_elements));
}

struct ui_detection_result *rectangles_to_ui_elements(
    const std::vector<cv::Rect> &rectangles,
    const char *detector_name)
{
    struct ui_detection_result *result =
        static_cast<struct ui_detection_result *>(
            calloc(1, sizeof(struct ui_detection_result)));
    if (!result)
        return nullptr;

    if (rectangles.empty()) {
        result->error = -1;
        snprintf(
            result->error_msg,
            sizeof(result->error_msg),
            "No UI elements detected by %s",
            detector_name);
        return result;
    }

    result->elements = static_cast<struct ui_element *>(
        calloc(rectangles.size(), sizeof(struct ui_element)));
    if (!result->elements) {
        result->error = -2;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "Memory allocation failed");
        free(result);
        return nullptr;
    }

    for (size_t i = 0; i < rectangles.size(); i++) {
        const cv::Rect &rect = rectangles[i];
        result->elements[i].x = rect.x;
        result->elements[i].y = rect.y;
        result->elements[i].w = rect.width;
        result->elements[i].h = rect.height;
        result->elements[i].name = nullptr;
        result->elements[i].role = static_cast<char *>(malloc(8));
        strcpy(result->elements[i].role, "element");
    }

    result->count = rectangles.size();
    result->error = 0;
    fprintf(stderr, "%s: Detected %zu UI elements\n",
            detector_name, result->count);
    return result;
}

void opencv_free_ui_elements_common(struct ui_detection_result *result)
{
    if (!result)
        return;

    if (result->elements) {
        for (size_t i = 0; i < result->count; i++) {
            free(result->elements[i].name);
            free(result->elements[i].role);
        }
        free(result->elements);
    }

    free(result);
}

#endif // HAVE_OPENCV
