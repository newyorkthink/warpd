# Changelog

## 2.3.0 - 2026-07-14

### Smart Hint

- Added strict X11 active-window matching for AT-SPI using PID, process ancestry, process name, title, WM_CLASS and window geometry.
- Prevented unrelated applications with similar geometry from being selected as the active AT-SPI window.
- Enabled the desktop accessibility service automatically before AT-SPI detection.
- Rejected browser and application results that expose only a single top-level frame.
- Kept OpenCV as an automatic fallback for terminals, custom-rendered applications and incomplete accessibility trees.
- Reduced noisy OpenCV results with geometry, edge-density and shape scoring and a maximum of 48 candidates.

### Text editing

- Read selected X11 text directly from the PRIMARY selection.
- Finalized drag selection before opening the editor.
- Improved editor focus and keyboard-grab reliability.
- Returned cleanly to Normal Mode when editing is cancelled.

### Packaging

- Added the stable Linux/X11 x86_64 AppImage build.
- Bundled OpenCV, xclip, the GTK3 edit dialog and the Fcitx5 GTK3 runtime.
- Added bundled-library verification and SHA-256 checksum generation.
- Changed releases from a rolling prerelease to versioned stable releases.
- Consolidated the X11 AT-SPI implementation into one production source file.
- Corrected source-install dependencies and repository URLs.

## 2.2.0

- Base version used for the Smart Hint and AppImage development cycle.
