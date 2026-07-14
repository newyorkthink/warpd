# warpd 2.3.0

A keyboard-driven mouse control tool with Smart Hint detection for Linux/X11.

This fork is based on the original [warpd](https://github.com/rvaiya/warpd) and adds accessibility-aware UI detection, OpenCV fallback, text-selection editing, an x86_64 AppImage build and improved Fcitx5 support.

## Stable AppImage

Download the latest stable release from:

- https://github.com/newyorkthink/warpd/releases/latest

Release asset:

```text
warpd-2.3.0-x86_64.AppImage
```

Install it manually:

```bash
chmod +x warpd-2.3.0-x86_64.AppImage
sudo install -m755 warpd-2.3.0-x86_64.AppImage /usr/local/bin/warpd
```

The AppImage targets X11 and bundles OpenCV, the GTK3 insert dialog, xclip and the Fcitx5 GTK3 runtime.

## Main modes

- **Hint Mode**: `Alt-Meta-x`
- **Grid Mode**: `Alt-Meta-g`
- **Normal Mode**: `Alt-Meta-c`
- **Smart Hint Mode**: `Alt-Meta-f`, or press `f` from Normal Mode

Normal Mode uses the standard warpd movement and click keys. Smart Hint assigns labels to detected UI elements so they can be selected directly from the keyboard.

## Smart Hint detection

Linux/X11 uses the following chain automatically:

1. Enable and query the desktop AT-SPI accessibility service.
2. Match the X11 active window to its AT-SPI window using PID, process family, title, WM_CLASS and geometry.
3. Collect real interactive controls such as links, buttons, inputs, menu items and tabs.
4. Reject root-only accessibility results that expose only the application frame.
5. Fall back to OpenCV visual detection when the application does not provide a usable accessibility tree.

OpenCV fallback is intentionally capped to avoid covering the screen with excessive labels. Applications do not require individual warpd configuration; unsupported or incomplete accessibility implementations fall back automatically.

## Text selection and editing

From Normal Mode:

- `v`: start drag selection
- `y`: copy the current selection
- `p`: paste clipboard contents
- `c`: copy and leave selection mode
- `i`: copy the selection, edit it in the bundled dialog, then paste the result

On X11, selected text is read from the PRIMARY selection instead of relying on injected copy shortcuts.

## Source installation

Automatic installation:

```bash
curl -fsSL https://raw.githubusercontent.com/newyorkthink/warpd/master/install.sh | sh
```

Install a specific release:

```bash
curl -fsSL https://raw.githubusercontent.com/newyorkthink/warpd/master/install.sh |
  WARPD_VERSION=v2.3.0 sh
```

Manual X11 build with OpenCV:

```bash
git clone https://github.com/newyorkthink/warpd.git
cd warpd
make -j"$(nproc)" OPENCV_ENABLE=1 DISABLE_WAYLAND=1
sudo make install
```

## Diagnostics

Run Smart Hint once and save the detector output:

```bash
warpd --smart-hint 2>&1 | tee /tmp/warpd-smart-hint.log
```

Typical successful paths are:

```text
Linux: AT-SPI found ... elements
```

or:

```text
Linux: AT-SPI detection failed ...
Linux: OpenCV found ... elements
```

A fallback is normal for terminals, canvas applications, browser pages without a complete renderer accessibility tree and other custom-drawn interfaces.

## Limitations

- The stable AppImage is Linux/X11 x86_64 only.
- AT-SPI accuracy depends on the application exposing a useful accessibility tree.
- OpenCV is broader but cannot always distinguish clickable controls from visually similar shapes.
- Wayland support remains limited by compositor security restrictions and is not included in the stable AppImage.

## Development

The project keeps one production X11 AT-SPI matcher under `src/platform/linux/atspi-x11-detector.c`. GitHub Actions builds the stable AppImage, verifies bundled libraries, generates a SHA-256 checksum and publishes a versioned GitHub Release.

See [CHANGELOG.md](CHANGELOG.md) and [warpd.1.md](warpd.1.md) for more details.

## License

See [LICENSE](LICENSE).
