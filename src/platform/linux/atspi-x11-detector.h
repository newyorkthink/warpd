/*
 * warpd - X11-aware AT-SPI window matching.
 */
#ifndef WARPD_ATSPI_X11_DETECTOR_H
#define WARPD_ATSPI_X11_DETECTOR_H

#include "../../platform.h"

struct ui_detection_result *atspi_x11_detect_ui_elements(void);
int atspi_x11_is_available(void);

#endif
