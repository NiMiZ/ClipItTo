#!/bin/sh
# Run on the Mac: builds, tests and packages the macOS disk image, and (with --upload) adds it to the
# GitHub release vX.Y.Z (create that release first with scripts/make-release.sh --publish on the PC).
#
#   scripts/make-release-macos.sh [--skip-tests] [--upload]
# Needs: Xcode command line tools, CMake, Ninja, Qt 6.5+ (QT_ROOT_DIR, or `brew install qt`), and `gh` for --upload.
# The translation libraries are built by scripts/build-translation-libs.sh (see docs/translation.md).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tests=1 upload=
for a in "$@"; do
    case $a in
        --skip-tests) tests= ;;
        --upload) upload=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done
version=$(sed -n 's/^ *VERSION \([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p' CMakeLists.txt | head -1)
fetch=${CLIPITTO_FETCH_DIR:-$HOME/fetchcontent}
if [ -n "$tests" ]; then
    cmake --preset macos -DFETCHCONTENT_BASE_DIR="$fetch"
    cmake --build --preset macos
    ctest --preset macos --output-on-failure
fi
cmake --preset macos-release -DFETCHCONTENT_BASE_DIR="$fetch"
cmake --build --preset macos-release
sh packaging/macos/package.sh build/macos-release build/macos-package
dmg=$(ls build/macos-package/ClipItTo-*-macos.dmg)
mkdir -p "dist/$version" && cp "$dmg" "dist/$version/"
echo "Disk image: dist/$version/$(basename "$dmg")"
if [ -n "$upload" ]; then
    gh release upload "v$version" "$dmg" --clobber
    echo "Uploaded. Rebuild SHA256SUMS to list it: scripts/make-release.sh --sums"
fi
