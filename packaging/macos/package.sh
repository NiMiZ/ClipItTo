#!/bin/sh
# Builds ClipItTo.app and ClipItTo-<version>-macos.dmg from a finished CMake build:
#   1. cmake --install into a staging folder (the bundle layout, see cmake/PackagingMac.cmake)
#   2. macdeployqt copies the Qt frameworks and plugins the binaries use into the bundle
#   3. an ad-hoc code signature (Apple Silicon refuses to run unsigned code; there is no
#      Developer ID certificate yet, so Gatekeeper still asks on the first launch)
#   4. checks: the signature holds, nothing points outside the bundle
#   5. the .dmg: the app next to a shortcut to /Applications
#
#   packaging/macos/package.sh BUILD_DIR OUT_DIR       needs Qt's macdeployqt on PATH
set -eu
build=${1:?usage: package.sh BUILD_DIR OUT_DIR}
out=${2:?usage: package.sh BUILD_DIR OUT_DIR}
mkdir -p "$out"
out=$(cd "$out" && pwd)
stage="$out/stage"
app="$stage/ClipItTo.app"

version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' "$(dirname "$0")/../../CMakeLists.txt" | head -n 1)
dmg="$out/ClipItTo-$version-macos.dmg"

# A fresh staging folder (the name is fixed here, never taken from the caller).
[ -d "$stage" ] && rm -r "$stage"
mkdir -p "$stage"
cmake --install "$build" --prefix "$stage"
[ -d "$app" ] || { echo "package.sh: cmake --install did not make $app" >&2; exit 1; }

command -v macdeployqt >/dev/null || { echo "package.sh: macdeployqt not found (Qt's bin folder on PATH)" >&2; exit 1; }
macdeployqt "$app" -executable="$app/Contents/MacOS/clipitto"

# ClipItTo makes no network connections (QtNetwork is only for its local control socket):
# the TLS and reachability plugins macdeployqt adds for it have no use here.
for plugins in tls networkinformation; do
    [ -d "$app/Contents/PlugIns/$plugins" ] && rm -r "$app/Contents/PlugIns/$plugins"
done

codesign --force --deep --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

# Nothing may point at the build machine: every library comes from inside the bundle or the system.
bad=0
for binary in "$app/Contents/MacOS/clipitto-daemon" "$app/Contents/MacOS/clipitto"; do
    # Library lines are indented; a universal binary also gets one unindented header per
    # architecture ("path (architecture arm64):"), which is not a library.
    if otool -L "$binary" | grep '^[[:space:]]' | awk '{print $1}' | grep -v -e '^@' -e '^/usr/lib/' -e '^/System/'; then
        echo "package.sh: $binary links outside the bundle (listed above)" >&2
        bad=1
    fi
done
[ "$bad" = 0 ] || exit 1
lipo -archs "$app/Contents/MacOS/clipitto-daemon"

# The disk image: the app, and /Applications to drag it onto.
dmgroot="$out/dmgroot"
[ -d "$dmgroot" ] && rm -r "$dmgroot"
mkdir -p "$dmgroot"
cp -R "$app" "$dmgroot/"
ln -s /Applications "$dmgroot/Applications"
[ -f "$dmg" ] && rm "$dmg"
# "hdiutil: create failed - Resource busy" happens now and then on the CI Macs (a volume of the last step is
# still being released): try again a few times.
n=0
until hdiutil create -volname "ClipItTo" -srcfolder "$dmgroot" -ov -format UDZO "$dmg"; do
    n=$((n + 1))
    [ "$n" -lt 4 ] || exit 1
    echo "hdiutil create failed (attempt $n); trying again" >&2
    sleep 15
done
rm -r "$dmgroot"
echo "built: $dmg ($(du -h "$dmg" | cut -f1)), app: $app"
