/*
 * warpd - AT-SPI global accessibility status control.
 */
#ifndef WARPD_ATSPI_STATUS_H
#define WARPD_ATSPI_STATUS_H

/*
 * Ensure org.a11y.Status.IsEnabled is true for the desktop session.
 * Returns non-zero when accessibility is enabled after the check.
 */
int atspi_ensure_global_accessibility(void);

#endif
