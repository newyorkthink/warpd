#if defined(HAVE_OPENCV)

/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * Linux OpenCV detector implementation
 * Unified detector that chooses between X11/Wayland at runtime
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "../../platform.h"

// Import config functions from C
extern const char *config_get(const char *key);
extern int config_get_int(const char *key);

#ifdef __cplusplus
}
#endif

#include "../../common/opencv_detector.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <algorithm>
#include <cstdlib>  // for atof

// Forward declarations for platform-specific screen capture
#ifdef WARPD_X
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
extern "C" Display *dpy; // From X.c
static cv::Mat capture_screenshot_x11();
static int capture_offset_x = 0;
static int capture_offset_y = 0;
static const char *capture_scope = "full screen";
#endif

#ifdef WARPD_WAYLAND
static cv::Mat capture_screenshot_wayland();
#endif

#ifdef WARPD_X
/**
 * Convert an XImage into an OpenCV BGRA image.
 */
static cv::Mat ximage_to_mat(XImage *ximg)
{
    if (!ximg)
        return cv::Mat();

    cv::Mat img(ximg->height, ximg->width, CV_8UC4);

    for (int y = 0; y < ximg->height; y++) {
        for (int x = 0; x < ximg->width; x++) {
            unsigned long pixel = XGetPixel(ximg, x, y);
            unsigned char b = (pixel & 0xFF);
            unsigned char g = (pixel >> 8) & 0xFF;
            unsigned char r = (pixel >> 16) & 0xFF;
            img.at<cv::Vec4b>(y, x) = cv::Vec4b(b, g, r, 255);
        }
    }

    return img;
}

/**
 * Read the EWMH active window selected by the window manager.
 */
static Window get_active_window_x11()
{
    Window root = DefaultRootWindow(dpy);
    Atom property = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (property == None)
        return None;

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(
        dpy, root, property, 0, 1, False, XA_WINDOW,
        &actual_type, &actual_format, &item_count, &bytes_after, &data);

    Window active = None;
    if (status == Success && data && item_count == 1 && actual_format == 32)
        active = *(Window *)data;

    if (data)
        XFree(data);

    return active;
}

/**
 * Capture a specific X11 window and record its absolute screen offset.
 */
static cv::Mat capture_window_x11(Window window, int offset_x, int offset_y,
                                  int width, int height, const char *scope)
{
    if (width <= 1 || height <= 1)
        return cv::Mat();

    XImage *ximg = XGetImage(dpy, window, 0, 0, width, height,
                             AllPlanes, ZPixmap);
    if (!ximg)
        return cv::Mat();

    cv::Mat img = ximage_to_mat(ximg);
    XDestroyImage(ximg);

    if (!img.empty()) {
        capture_offset_x = offset_x;
        capture_offset_y = offset_y;
        capture_scope = scope;
    }

    return img;
}

/**
 * Capture the current active X11 window. Fall back to the full root window when
 * the window manager does not expose a valid _NET_ACTIVE_WINDOW.
 */
static cv::Mat capture_screenshot_x11()
{
    Window root = DefaultRootWindow(dpy);
    capture_offset_x = 0;
    capture_offset_y = 0;
    capture_scope = "full screen";

    Window active = get_active_window_x11();
    if (active != None && active != root) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, active, &attrs) &&
            attrs.map_state == IsViewable && attrs.width > 1 && attrs.height > 1) {
            int root_x = 0;
            int root_y = 0;
            Window child = None;

            if (XTranslateCoordinates(dpy, active, root, 0, 0,
                                      &root_x, &root_y, &child)) {
                cv::Mat active_image = capture_window_x11(
                    active, root_x, root_y, attrs.width, attrs.height,
                    "active window");

                if (!active_image.empty()) {
                    fprintf(stderr,
                            "OpenCV: Capturing active X11 window at (%d,%d), size %dx%d\n",
                            root_x, root_y, attrs.width, attrs.height);
                    return active_image;
                }
            }
        }
    }

    XWindowAttributes root_attrs;
    if (!XGetWindowAttributes(dpy, root, &root_attrs))
        return cv::Mat();

    fprintf(stderr,
            "OpenCV: Active window unavailable; falling back to full screen %dx%d\n",
            root_attrs.width, root_attrs.height);

    return capture_window_x11(root, 0, 0, root_attrs.width, root_attrs.height,
                              "full screen");
}
#endif

#ifdef WARPD_WAYLAND
/**
 * Capture screenshot using Wayland and convert to cv::Mat
 * TODO: Implement proper Wayland screen capture
 */
static cv::Mat capture_screenshot_wayland()
{
    // TODO: Implement Wayland screen capture using:
    // - wlr-screencopy protocol for wlroots compositors
    // - xdg-desktop-portal for GNOME/KDE
    // - Direct compositor-specific APIs

    fprintf(stderr, "WARNING: Wayland OpenCV screen capture not yet implemented\n");
    return cv::Mat();
}
#endif

/**
 * Capture screenshot using the appropriate method for the current session
 */
static cv::Mat capture_screenshot_linux()
{
#ifdef WARPD_X
    if (dpy) {
        return capture_screenshot_x11();
    }
#endif

#ifdef WARPD_WAYLAND
    // TODO: Add runtime Wayland detection
    return capture_screenshot_wayland();
#endif

    fprintf(stderr, "ERROR: No screen capture method available\n");
    return cv::Mat();
}

// C interface functions
extern "C" {

/**
 * Check if OpenCV is available
 */
int opencv_is_available(void)
{
#ifdef WARPD_X
    if (dpy) {
        return 1; // X11 OpenCV is available
    }
#endif

#ifdef WARPD_WAYLAND
    // TODO: Check if Wayland screen capture is available
    fprintf(stderr, "WARNING: Wayland OpenCV detector not fully implemented\n");
    return 0; // Disabled until implementation is complete
#endif

    return 0;
}

/**
 * Detect UI elements using OpenCV on Linux
 * Supports configurable detection modes: strict, relaxed, auto
 */
struct ui_detection_result *opencv_detect_ui_elements(void)
{
    struct ui_detection_result *result =
        (struct ui_detection_result *)calloc(1, sizeof(struct ui_detection_result));
    if (!result)
        return NULL;

    try {
        const char *backend = "Unknown";
#ifdef WARPD_X
        if (dpy) backend = "X11";
#endif
#ifdef WARPD_WAYLAND
        if (!dpy) backend = "Wayland";
#endif

        fprintf(stderr, "\n");
        fprintf(stderr, "========================================\n");
        fprintf(stderr, "  OpenCV UI Detection Debug Output (%s)\n", backend);
        fprintf(stderr, "========================================\n");

        // Capture screenshot
        cv::Mat screenshot = capture_screenshot_linux();
        if (screenshot.empty()) {
            result->error = -1;
            snprintf(result->error_msg, sizeof(result->error_msg),
                     "Failed to capture %s screenshot", backend);
            return result;
        }

#ifdef WARPD_X
        fprintf(stderr,
                "\nStep 0: Captured %s %s (%dx%d), screen offset=(%d,%d)\n",
                backend, capture_scope, screenshot.cols, screenshot.rows,
                capture_offset_x, capture_offset_y);
#else
        fprintf(stderr, "\nStep 0: Captured %s screenshot (%dx%d)\n",
                backend, screenshot.cols, screenshot.rows);
#endif

        // Detect rectangles using OpenCV
        std::vector<cv::Rect> rects = detect_rectangles(screenshot);

        fprintf(stderr, "\n");

        if (rects.empty()) {
            result->error = -2;
            snprintf(result->error_msg, sizeof(result->error_msg),
                     "No UI elements detected");
            fprintf(stderr, "❌ ERROR: No UI elements detected after filtering!\n");
            fprintf(stderr, "========================================\n\n");
            return result;
        }

        // Step 6: Deduplicate
        fprintf(stderr, "Step 6: Deduplicating rectangles\n");
        fprintf(stderr, "  Before dedup: %zu\n", rects.size());
        rects = deduplicate_rectangles(rects);
        fprintf(stderr, "  After dedup: %zu\n", rects.size());

        // Limit to MAX_UI_ELEMENTS
        if (rects.size() > MAX_UI_ELEMENTS) {
            fprintf(stderr, "  Limited to: %d (MAX_UI_ELEMENTS)\n", MAX_UI_ELEMENTS);
            rects.resize(MAX_UI_ELEMENTS);
        }

        // Allocate result
        result->elements = (struct ui_element *)calloc(rects.size(), sizeof(struct ui_element));
        if (!result->elements) {
            result->error = -3;
            snprintf(result->error_msg, sizeof(result->error_msg),
                     "Memory allocation failed");
            return result;
        }

        // Copy rectangles to result. OpenCV coordinates are relative to the
        // captured window, so translate them back to absolute screen positions.
        for (size_t i = 0; i < rects.size(); i++) {
#ifdef WARPD_X
            result->elements[i].x = rects[i].x + capture_offset_x;
            result->elements[i].y = rects[i].y + capture_offset_y;
#else
            result->elements[i].x = rects[i].x;
            result->elements[i].y = rects[i].y;
#endif
            result->elements[i].w = rects[i].width;
            result->elements[i].h = rects[i].height;
            result->elements[i].name = strdup("UI Element");
            result->elements[i].role = strdup("button");
        }

        result->count = rects.size();
        result->error = 0;

        fprintf(stderr, "\n");
#ifdef WARPD_X
        fprintf(stderr, "✅ SUCCESS: Detected %zu UI elements (%s %s)\n",
                result->count, backend, capture_scope);
#else
        fprintf(stderr, "✅ SUCCESS: Detected %zu UI elements (%s)\n",
                result->count, backend);
#endif
        fprintf(stderr, "========================================\n\n");

    } catch (const cv::Exception &e) {
        result->error = -4;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "OpenCV error: %s", e.what());
        fprintf(stderr, "❌ OpenCV error: %s\n", e.what());
    } catch (...) {
        result->error = -5;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "Unknown error in OpenCV detection");
        fprintf(stderr, "❌ Unknown error in OpenCV detection\n");
    }

    return result;
}

/**
 * Free OpenCV detection result
 */
void opencv_free_ui_elements(struct ui_detection_result *result)
{
    opencv_free_ui_elements_common(result);
}

} // extern "C"

#endif // HAVE_OPENCV
