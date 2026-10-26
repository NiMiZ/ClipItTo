#!/bin/sh
# Builds ClipItTo-<version>-x86_64.AppImage: a release build installed into an AppDir, then
# linuxdeploy with its Qt plugin bundles Qt (xcb and Wayland), toml++, QtKeychain and the rest.
# The tools are downloaded once, pinned by SHA-256. No FUSE needed.
#
#   packaging/appimage/build-appimage.sh [BUILD_DIR]     default: build/appimage
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(mkdir -p "${1:-$root/build/appimage}" && cd "${1:-$root/build/appimage}" && pwd)
tools=$build/tools
appdir=$build/AppDir

ld_version=1-alpha-20251107-1
ld_sha256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
qt_version=1-alpha-20250213-1
qt_sha256=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724

fetch() { # name url sha256
    if ! { [ -f "$tools/$1" ] && echo "$3  $tools/$1" | sha256sum -c --quiet >/dev/null 2>&1; }; then
        curl -fsSL -o "$tools/$1" "$2"
        echo "$3  $tools/$1" | sha256sum -c --quiet
    fi
    chmod +x "$tools/$1"
}
mkdir -p "$tools"
fetch linuxdeploy-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/$ld_version/linuxdeploy-x86_64.AppImage" "$ld_sha256"
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$qt_version/linuxdeploy-plugin-qt-x86_64.AppImage" \
    "$qt_sha256"

cmake -S "$root" -B "$build/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF -DCLIPITTO_SYSTEMD_USER_UNIT_DIR=/usr/lib/systemd/user
cmake --build "$build/cmake"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build/cmake"

version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -n 1)
export APPIMAGE_EXTRACT_AND_RUN=1 # the tools are AppImages themselves: run them without FUSE
export QMAKE="${QMAKE:-$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake6)}"
# xcb comes by default; Wayland, and offscreen for headless runs (QT_QPA_PLATFORM=offscreen).
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
export LINUXDEPLOY_OUTPUT_VERSION="$version"
cd "$build"
"$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" --plugin qt --output appimage \
    --custom-apprun "$root/packaging/appimage/AppRun" \
    --executable "$appdir/usr/bin/clipitto-daemon" --executable "$appdir/usr/bin/clipitto" \
    --desktop-file "$appdir/usr/share/applications/io.github.nimiz.ClipItTo.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/io.github.nimiz.ClipItTo.svg"
ls -l "$build"/ClipItTo-*.AppImage
