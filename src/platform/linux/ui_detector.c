/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * Linux UI Element Detector using AT-SPI with OpenCV fallback
 */

#include "../../platform.h"
#include "../../common/detector_orchestrator.h"
#include "../../common/opencv_detector.h"
#include "atspi-detector.h"
#include "atspi-status.h"
#include "atspi-x11-detector.h"
#include <at-spi-2.0/atspi/atspi.h>
#include <glib-2.0/glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Convert legacy AT-SPI ElementInfo to platform ui_element.
 * This path remains available for non-X11 builds.
 */
static void convert_atspi_element(ElementInfo *src, struct ui_element *dest)
{
	dest->x = src->x;
	dest->y = src->y;
	dest->w = src->w;
	dest->h = src->h;
	dest->name = src->name ? strdup(src->name) : NULL;
	dest->role = src->role ? strdup(src->role) : NULL;
}

static struct ui_detection_result *legacy_atspi_detect_ui_elements(void)
{
	struct ui_detection_result *result = calloc(1, sizeof(*result));
	GSList *element_list;
	GSList *iter;
	size_t count;

	if (!result)
		return NULL;

	atspi_init_detector();
	element_list = detect_elements();
	if (!element_list) {
		snprintf(
		    result->error_msg,
		    sizeof(result->error_msg),
		    "No active window or AT-SPI not available");
		result->error = -1;
		return result;
	}

	count = g_slist_length(element_list);
	if (count == 0) {
		result->error = -2;
		snprintf(
		    result->error_msg,
		    sizeof(result->error_msg),
		    "No interactive elements detected");
		free_detector_resources();
		return result;
	}

	result->elements = calloc(count, sizeof(struct ui_element));
	if (!result->elements) {
		result->error = -3;
		snprintf(
		    result->error_msg,
		    sizeof(result->error_msg),
		    "Memory allocation failed");
		free_detector_resources();
		return result;
	}

	iter = element_list;
	for (size_t i = 0; i < count && iter; i++, iter = iter->next) {
		ElementInfo *element = (ElementInfo *)iter->data;
		if (element)
			convert_atspi_element(element, &result->elements[i]);
	}

	result->count = count;
	result->error = 0;
	free_detector_resources();
	return result;
}

static struct ui_detection_result *atspi_detect_ui_elements(void)
{
#ifdef WARPD_X
	/*
	 * i3/Chromium-family applications often omit ATSPI_STATE_ACTIVE. Match the
	 * X11 _NET_ACTIVE_WINDOW to AT-SPI by PID, title, WM_CLASS and geometry.
	 */
	return atspi_x11_detect_ui_elements();
#else
	return legacy_atspi_detect_ui_elements();
#endif
}

static int atspi_is_available(void)
{
#ifdef WARPD_X
	return atspi_x11_is_available();
#else
	return 1;
#endif
}

static void atspi_free_ui_elements(struct ui_detection_result *result)
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

struct ui_detection_result *linux_detect_ui_elements(void)
{
	detector_strategy_t strategies[] = {
		{
			.name = "AT-SPI",
			.is_available = atspi_is_available,
			.detect = atspi_detect_ui_elements,
			.free_result = atspi_free_ui_elements,
			.min_elements = 0,
		},
		{
			.name = "OpenCV",
			.is_available = opencv_is_available,
			.detect = opencv_detect_ui_elements,
			.free_result = opencv_free_ui_elements,
			.min_elements = 0,
		},
	};

	/*
	 * Chromium-family browsers only initialize their Linux accessibility backend
	 * when the desktop accessibility status is enabled. The org.a11y.Status
	 * setter also persists the corresponding toolkit-accessibility GSettings key.
	 */
	atspi_ensure_global_accessibility();

	return detector_orchestrator_run(strategies, 2, "Linux");
}

void linux_free_ui_elements(struct ui_detection_result *result)
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
