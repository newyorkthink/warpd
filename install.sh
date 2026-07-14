#!/usr/bin/env bash

# Install warpd from source with X11 Smart Hint support.
#
# Latest master:
#   curl -fsSL https://raw.githubusercontent.com/newyorkthink/warpd/master/install.sh | sh
#
# Specific release:
#   curl -fsSL https://raw.githubusercontent.com/newyorkthink/warpd/master/install.sh | WARPD_VERSION=v2.3.0 sh

set -euo pipefail

REPOSITORY="https://github.com/newyorkthink/warpd.git"
VERSION="${WARPD_VERSION:-master}"
BUILD_DIR=""
TEMP_DIR=""
CLEANUP_NEEDED=false

cleanup() {
    if [ "$CLEANUP_NEEDED" = true ] && [ -n "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
    fi
}
trap cleanup EXIT

if [ "${EUID:-$(id -u)}" -eq 0 ]; then
    echo "Error: do not run this installer as root." >&2
    echo "The installer invokes sudo only for dependencies and installation." >&2
    exit 1
fi

install_dependencies() {
    echo "Installing build dependencies..."

    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update
        sudo apt-get install -y \
            git make gcc g++ pkg-config \
            libxi-dev libxinerama-dev libxft-dev libxfixes-dev \
            libxext-dev libxtst-dev libx11-dev \
            libatspi2.0-dev libdbus-1-dev libglib2.0-dev \
            libopencv-dev zenity xclip
    elif command -v pacman >/dev/null 2>&1; then
        sudo pacman -S --needed --noconfirm \
            git make gcc pkgconf \
            libxi libxinerama libxft libxfixes libxext libxtst libx11 \
            at-spi2-core dbus glib2 opencv zenity xclip
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y \
            git make gcc gcc-c++ pkgconf-pkg-config \
            libXi-devel libXinerama-devel libXft-devel libXfixes-devel \
            libXext-devel libXtst-devel libX11-devel \
            at-spi2-core-devel dbus-devel glib2-devel opencv-devel \
            zenity xclip
    else
        echo "Error: unsupported distribution." >&2
        echo "Install the X11, AT-SPI, GLib, OpenCV, Zenity and xclip dependencies manually." >&2
        exit 1
    fi
}

prepare_source() {
    if [ -f Makefile ] && [ -f src/warpd.c ] && [ -d src ]; then
        BUILD_DIR="$PWD"
        echo "Using source directory: $BUILD_DIR"
        return
    fi

    TEMP_DIR="$(mktemp -d)"
    CLEANUP_NEEDED=true

    if [ "$VERSION" = master ] || [ "$VERSION" = latest ]; then
        git clone --depth 1 "$REPOSITORY" "$TEMP_DIR/warpd"
    else
        git clone --depth 1 --branch "$VERSION" "$REPOSITORY" "$TEMP_DIR/warpd"
    fi

    BUILD_DIR="$TEMP_DIR/warpd"
}

install_dependencies
prepare_source

cd "$BUILD_DIR"

echo "Building warpd from: $VERSION"
make clean >/dev/null 2>&1 || true
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" \
    OPENCV_ENABLE=1 \
    DISABLE_WAYLAND=1

sudo make install

printf '\nInstalled: '
warpd --version 2>/dev/null || echo "warpd"
echo "Run 'man warpd' for usage details."
