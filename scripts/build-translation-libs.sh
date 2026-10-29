#!/bin/sh
# Builds the libraries behind offline translation, CTranslate2 (MIT) and SentencePiece (Apache-2.0), as static
# libraries in a prefix, the way ClipItTo's CMake wants them (-DCLIPITTO_TRANSLATE_PREFIX=PREFIX):
#   PREFIX/include/ctranslate2/..., PREFIX/include/sentencepiece_processor.h
#   PREFIX/lib/libctranslate2_all.a   CTranslate2 and its helper libraries merged into one archive
#   PREFIX/lib/libsentencepiece.a
#   PREFIX/licenses/                  the licence texts, to ship with the app
# Neither library opens a network connection; the sources are fetched here, at build time, from GitHub at
# pinned tags (the commit is checked).
#
#   scripts/build-translation-libs.sh PREFIX              Linux, or macOS for this machine's architecture
#   scripts/build-translation-libs.sh PREFIX universal    macOS: arm64 and x86_64 in one archive
# CLIPITTO_TRANSLATE_WORK: where the sources and build folders go (default: PREFIX-work).
set -eu

prefix=${1:?usage: build-translation-libs.sh PREFIX [universal]}
mode=${2:-native}
prefix=$(mkdir -p "$prefix" && cd "$prefix" && pwd)
work=${CLIPITTO_TRANSLATE_WORK:-$prefix-work}
mkdir -p "$work"

ct2_tag=v4.5.0
ct2_commit=383d063daf0bf338a22b491864cb2018eb8efd15
sp_tag=v0.2.0
sp_commit=17d7580d6407802f85855d2cc9190634e2c95624
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
os=$(uname -s)

fetch() { # name url tag commit
    if [ ! -d "$work/$1/.git" ]; then
        n=0
        until git clone -q --depth 1 --branch "$3" "$2" "$work/$1"; do
            n=$((n + 1)); [ "$n" -lt 4 ] || exit 1
            echo "git clone failed (attempt $n); trying again" >&2; rm -rf "$work/$1"; sleep 20
        done
    fi
    got=$(git -C "$work/$1" rev-parse HEAD)
    [ "$got" = "$4" ] || { echo "$1: expected commit $4, got $got" >&2; exit 1; }
    n=0
    until git -C "$work/$1" submodule update -q --init --recursive --depth 1; do
        n=$((n + 1)); [ "$n" -lt 4 ] || exit 1
        echo "submodule update failed (attempt $n); trying again" >&2; sleep 20
    done
}
fetch ctranslate2 https://github.com/OpenNMT/CTranslate2.git "$ct2_tag" "$ct2_commit"
fetch sentencepiece https://github.com/google/sentencepiece.git "$sp_tag" "$sp_commit"

# Merges static archives into one: ar's script mode on Linux, libtool on macOS.
merge() { # out in...
    out=$1; shift
    rm -f "$out"
    if [ "$os" = Darwin ]; then
        libtool -static -o "$out" "$@"
    else
        { echo "CREATE $out"; for a in "$@"; do echo "ADDLIB $a"; done; echo "SAVE"; echo "END"; } | ar -M
    fi
}

build_ct2() { # arch|native  -> $work/ct2-<arch>/merged.a
    arch=$1
    dir=$work/ct2-$arch
    extra=""
    if [ "$os" = Darwin ]; then
        extra="-DWITH_ACCELERATE=ON"
        if [ "$arch" != native ]; then
            # The libraries pick their CPU code from CMAKE_SYSTEM_PROCESSOR, which is the machine's, not the
            # target's: a toolchain file makes the target explicit (x86_64 built on an arm64 Mac).
            cat > "$work/macos-$arch.cmake" <<TOOLCHAIN
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR $arch)
TOOLCHAIN
            # Naming the system makes CMake treat this as a cross build, which drops the default SDK path.
            extra="$extra -DCMAKE_TOOLCHAIN_FILE=$work/macos-$arch.cmake -DCMAKE_OSX_ARCHITECTURES=$arch"
            extra="$extra -DCMAKE_OSX_SYSROOT=$(xcrun --sdk macosx --show-sdk-path)"
        fi
    fi
    # shellcheck disable=SC2086
    cmake -S "$work/ctranslate2" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_CLI=OFF -DWITH_MKL=OFF -DWITH_DNNL=OFF -DWITH_RUY=ON -DOPENMP_RUNTIME=NONE \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 $extra
    cmake --build "$dir" -j "$jobs"
    libs=$(find "$dir" -name '*.a' ! -name 'libctranslate2.a' | sort)
    # shellcheck disable=SC2086
    merge "$dir/merged.a" "$dir/libctranslate2.a" $libs
}

mkdir -p "$prefix/lib" "$prefix/include" "$prefix/licenses"
if [ "$mode" = universal ] && [ "$os" = Darwin ]; then
    build_ct2 arm64
    build_ct2 x86_64
    lipo -create "$work/ct2-arm64/merged.a" "$work/ct2-x86_64/merged.a" -output "$prefix/lib/libctranslate2_all.a"
    sp_flags="-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
else
    build_ct2 native
    cp "$work/ct2-native/merged.a" "$prefix/lib/libctranslate2_all.a"
    sp_flags=""
fi
cp -R "$work/ctranslate2/include/." "$prefix/include/" # ctranslate2, half_float, nlohmann

cmake -S "$work/sentencepiece" -B "$work/sp" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPM_ENABLE_SHARED=OFF \
    -DSPM_BUILD_TEST=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$work/sp-install" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$sp_flags"
cmake --build "$work/sp" -j "$jobs"
cmake --install "$work/sp" >/dev/null
cp "$work/sp-install/lib/libsentencepiece.a" "$prefix/lib/"
cp "$work/sp-install/include/sentencepiece_processor.h" "$prefix/include/"

# The licences that travel with the app: CTranslate2 (MIT) and what it bundles, SentencePiece (Apache-2.0).
cp "$work/ctranslate2/LICENSE" "$prefix/licenses/CTranslate2-LICENSE.txt"
cp "$work/ctranslate2/third_party/cpu_features/LICENSE" "$prefix/licenses/cpu_features-LICENSE.txt"
cp "$work/ctranslate2/third_party/ruy/LICENSE" "$prefix/licenses/ruy-LICENSE.txt"
cp "$work/ctranslate2/third_party/spdlog/LICENSE" "$prefix/licenses/spdlog-LICENSE.txt"
cp "$work/sentencepiece/LICENSE" "$prefix/licenses/SentencePiece-LICENSE.txt"
[ -f "$work/ctranslate2/third_party/ruy/third_party/cpuinfo/LICENSE" ] &&
    cp "$work/ctranslate2/third_party/ruy/third_party/cpuinfo/LICENSE" "$prefix/licenses/cpuinfo-LICENSE.txt"
echo "Translation libraries are in $prefix"
