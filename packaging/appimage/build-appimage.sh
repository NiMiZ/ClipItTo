#!/bin/sh
# Builds ClipItTo-<version>-x86_64.AppImage: a release build installed into an AppDir, then
# linuxdeploy with its Qt plugin bundles Qt (xcb and Wayland), toml++, QtKeychain and the rest.
# The tools (and the AppImage runtime appimagetool would otherwise fetch on every run) are
# downloaded once, pinned by SHA-256, with retries. No FUSE needed.
#
#   packaging/appimage/build-appimage.sh [BUILD_DIR]     default: build/appimage
# CLIPITTO_FETCH_DIR: where CMake keeps the dependency sources it downloads (CI caches it).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(mkdir -p "${1:-$root/build/appimage}" && cd "${1:-$root/build/appimage}" && pwd)
tools=$build/tools
appdir=$build/AppDir

ld_version=1-alpha-20251107-1
ld_sha256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
qt_version=1-alpha-20250213-1
qt_sha256=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
runtime_version=20251108 # AppImage/type2-runtime
runtime_sha256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

tessdata_version=4.1.0 # tesseract-ocr/tessdata_fast (Apache-2.0): English text recognition data
tessdata_sha256=7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2

fetch() { # name url sha256
    if ! { [ -f "$tools/$1" ] && echo "$3  $tools/$1" | sha256sum -c --quiet >/dev/null 2>&1; }; then
        curl -fsSL --retry 5 --retry-delay 5 --retry-all-errors -o "$tools/$1" "$2"
        echo "$3  $tools/$1" | sha256sum -c --quiet
    fi
    chmod +x "$tools/$1"
}
mkdir -p "$tools"
fetch eng.traineddata \
    "https://github.com/tesseract-ocr/tessdata_fast/raw/$tessdata_version/eng.traineddata" "$tessdata_sha256"
fetch runtime-x86_64 \
    "https://github.com/AppImage/type2-runtime/releases/download/$runtime_version/runtime-x86_64" "$runtime_sha256"
fetch linuxdeploy-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/$ld_version/linuxdeploy-x86_64.AppImage" "$ld_sha256"
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$qt_version/linuxdeploy-plugin-qt-x86_64.AppImage" \
    "$qt_sha256"

cmake -S "$root" -B "$build/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF -DCLIPITTO_SYSTEMD_USER_UNIT_DIR=/usr/lib/systemd/user \
    ${CLIPITTO_FETCH_DIR:+-DFETCHCONTENT_BASE_DIR="$CLIPITTO_FETCH_DIR"}
cmake --build "$build/cmake"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build/cmake"
# Text recognition (Tesseract is bundled by linuxdeploy with the other libraries): the English data.
mkdir -p "$appdir/usr/share/tessdata"
cp "$tools/eng.traineddata" "$appdir/usr/share/tessdata/eng.traineddata"
chmod 644 "$appdir/usr/share/tessdata/eng.traineddata"

version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -n 1)
export APPIMAGE_EXTRACT_AND_RUN=1 # the tools are AppImages themselves: run them without FUSE
export LDAI_RUNTIME_FILE="$tools/runtime-x86_64" # the pinned runtime: appimagetool downloads none
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
