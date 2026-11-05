#!/bin/sh
# Builds the release files on this PC (WSL + Windows) into dist/<version>/, and can publish them.
#
#   scripts/make-release.sh [--skip-tests] [--only linux|windows|deb|appimage|snap] [--publish]
#
# Linux (here, in WSL): the .deb, the AppImage and the snap, each run once as a smoke test.
# Windows (through powershell.exe): build, tests and the Inno Setup installer; needs Visual Studio
#   Build Tools (C++), Qt 6.5.3 for MSVC, vcpkg and Inno Setup (see docs/releasing.md).
# macOS cannot be built here: run scripts/make-release-macos.sh on the Mac; it adds the .dmg to the
#   same GitHub release (`gh release upload`).
# --publish: SHA256SUMS, then `gh release create vX.Y.Z` (notes from docs/releases/vX.Y.Z.md) with the files
#   and install.sh / install.ps1. The tag vX.Y.Z must already be pushed (a tag run on GitHub that only
#   tests, see docs/releasing.md). Nothing is published without the flag.
# --sums: after the Mac added its .dmg, rebuild SHA256SUMS from the files on the release and upload it.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

tests=1 only= publish= sums=
while [ $# -gt 0 ]; do
    case $1 in
        --skip-tests) tests= ;;
        --only) only=$2; shift ;;
        --publish) publish=1 ;;
        --sums) sums=1 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

version=$(sed -n 's/^ *VERSION \([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p' CMakeLists.txt | head -1)
[ -n "$version" ] || { echo "no VERSION in CMakeLists.txt" >&2; exit 1; }
dist=$root/dist/$version
mkdir -p "$dist"
echo "== ClipItTo $version -> $dist"

step() { echo; echo "== $*"; }

if [ -n "$sums" ]; then
    tmp=$(mktemp -d)
    gh release download "v$version" --dir "$tmp" --pattern '*' --clobber
    (cd "$tmp" && rm -f SHA256SUMS && sha256sum -- * > SHA256SUMS && cat SHA256SUMS)
    gh release upload "v$version" "$tmp/SHA256SUMS" --clobber
    rm -rf "$tmp"
    exit 0
fi

build_tests() {
    export CLIPITTO_FETCH_DIR=${CLIPITTO_FETCH_DIR:-$HOME/fetchcontent}
    step "Linux: build and test (ASan/UBSan)"
    cmake --preset asan -DFETCHCONTENT_BASE_DIR="$CLIPITTO_FETCH_DIR"
    cmake --build --preset asan
    ctest --preset asan --output-on-failure
}

build_deb() {
    export CLIPITTO_FETCH_DIR=${CLIPITTO_FETCH_DIR:-$HOME/fetchcontent}
    step "Linux: .deb"
    cmake --preset deb -DFETCHCONTENT_BASE_DIR="$CLIPITTO_FETCH_DIR"
    cmake --build --preset deb
    rm -f build/deb/clipitto_*.deb "$dist"/clipitto_*.deb # an older one would be shipped along
    (cd build/deb && cpack)
    cp "build/deb/clipitto_${version}_"*.deb "$dist/"
    dpkg-deb -I "$dist/clipitto_${version}_"*.deb | head -12
}

build_appimage() {
    export CLIPITTO_FETCH_DIR=${CLIPITTO_FETCH_DIR:-$HOME/fetchcontent}
    step "Linux: AppImage"
    # linuxdeploy makes symlinks, which a Windows drive mounted in WSL (/mnt/c, /mnt/i...) refuses: build on a Linux
    # file system.
    appbuild=${CLIPITTO_APPIMAGE_BUILD:-$HOME/.cache/clipitto-release/appimage}
    mkdir -p "$appbuild"
    rm -f "$appbuild"/ClipItTo-*-x86_64.AppImage "$dist"/ClipItTo-*-x86_64.AppImage
    sh packaging/appimage/build-appimage.sh "$appbuild"
    cp "$appbuild/ClipItTo-${version}-x86_64.AppImage" "$dist/"
    APPIMAGE_EXTRACT_AND_RUN=1 "$dist/ClipItTo-${version}-x86_64.AppImage" --version
}

build_snap() {
    step "Linux: snap"
    rm -f "$dist"/clipitto_*.snap
    snapcraft pack --output "$dist/"
}

build_linux() {
    [ -z "$tests" ] || build_tests
    build_deb
    build_appimage
    build_snap
}

build_windows() {
    step "Windows: build, test, installer"
    winroot=$(wslpath -w "$root")
    flags=
    [ -n "$tests" ] || flags=-NoTests
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$winroot\\scripts\\make-release-windows.ps1" \
        -Version "$version" -OutputDir "$(wslpath -w "$dist")" $flags
}

case $only in
    '' | linux) build_linux ;;
    deb) build_deb ;;
    appimage) build_appimage ;;
    snap) build_snap ;;
esac
case $only in
    '' | windows) build_windows ;;
esac

step "Files"
ls -l "$dist"

if [ -n "$publish" ]; then
    step "Publish v$version"
    cp install.sh install.ps1 "$dist/"
    (cd "$dist" && sha256sum *.deb *.exe *.AppImage *.snap install.sh install.ps1 $(ls *.dmg 2>/dev/null) > SHA256SUMS && cat SHA256SUMS)
    notes=docs/releases/v$version.md
    [ -f "$notes" ] || { echo "missing $notes" >&2; exit 1; }
    git ls-remote --exit-code --tags origin "refs/tags/v$version" >/dev/null ||
        { echo "tag v$version is not pushed yet" >&2; exit 1; }
    gh release create "v$version" --title "ClipItTo $version" --notes-file "$notes" --generate-notes \
        "$dist"/*.deb "$dist"/*.exe "$dist"/*.AppImage "$dist"/*.snap "$dist"/install.sh "$dist"/install.ps1 "$dist"/SHA256SUMS
fi
