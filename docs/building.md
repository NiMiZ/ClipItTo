# Building from source

Back to the [README](../README.md).

Presets: `debug`, `release`, `asan` (address/UB sanitizers, warnings as errors), `deb`
(installs under `/usr`, for `cpack`), `windows`, `macos`, `macos-release`. CI
([.github/workflows/ci.yml](../.github/workflows/ci.yml)) builds and tests all three platforms
and builds the Debian package, AppImage and snap.

## Linux

Needs Ubuntu 24.04+ or equivalent: Qt 6.4+, SQLite 3.35+, toml++ 3.

```bash
# 1. Dependencies
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-wayland libgl-dev \
  libsqlite3-dev libtomlplusplus-dev libx11-dev libxtst-dev libglib2.0-bin qtkeychain-qt6-dev wl-clipboard

# 2. Build and test
git clone https://github.com/NiMiZ/ClipItTo.git && cd ClipItTo
cmake --preset release && cmake --build --preset release
ctest --preset release --output-on-failure    # gjs and dbus-run-session enable the extension and D-Bus tests

# 3. Run without installing
build/release/src/daemon/clipitto-daemon &
build/release/src/cli/clipitto --toggle     # bind this command to a shortcut to use it daily
build/release/src/cli/clipitto --status     # which capture backend is active
```

To install, build the package and install it (then `clipitto-setup` as above), or install
into `~/.local` without root (needs `~/.local/bin` on `PATH`):

```bash
cmake --preset deb && cmake --build --preset deb
(cd build/deb && cpack) && sudo apt install ./build/deb/clipitto_0.7.0_amd64.deb

cmake --preset release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build --preset release && cmake --install build/release && clipitto-setup
```

On GNOME Wayland, install the extension from the source tree, log out and back in, then
enable it:

```bash
gnome-extensions pack --force gnome-extension/clipitto@nimiz.github.io
gnome-extensions install --force clipitto@nimiz.github.io.shell-extension.zip
# log out and back in, then:
gnome-extensions enable clipitto@nimiz.github.io
```

The daemon runs as the `clipitto.service` systemd user unit and is also started on demand
through D-Bus (`io.github.nimiz.ClipItTo`, see [the interface](../src/ipc/io.github.nimiz.ClipItTo.xml)).
Search benchmark: `build/debug/tests/bench_search`.

## Windows

Needs Visual Studio 2022 or newer, or its Build Tools, with the *Desktop
development with C++* workload; Qt 6.5+ for MSVC; and [vcpkg](https://vcpkg.io), which builds
toml++ from [vcpkg.json](../vcpkg.json). The script finds them (Qt and vcpkg in `D:\` or `C:\`, or
`QT_ROOT_DIR` / `VCPKG_ROOT`), sets up the compiler environment, builds and runs the tests:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1          # build + tests
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Stage   # + a runnable copy in build\windows-stage
```

By hand, from a *Developer PowerShell for VS*:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"; $env:QT_ROOT_DIR = "C:\Qt\6.5.3\msvc2019_64"
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
cmake --install build/windows --prefix $PWD\stage   # runnable folder (absolute path required)
iscc /DStageDir=$PWD\stage packaging\windows\clipitto.iss   # optional installer (Inno Setup 6)
```

## macOS

Needs Xcode's command line tools, CMake, Ninja and Qt 6.5+ for macOS.
A Mac has none of these by default. Install the tools with any one of:

- **Homebrew:** `xcode-select --install`, then `brew install cmake ninja`.
- **No Homebrew:** download the CMake `.dmg` from cmake.org/download (macOS universal) and
  drag it to Applications; run `sudo "/Applications/CMake.app/Contents/bin/cmake-gui" --install`
  once to put `cmake` on the PATH. Ninja is a single binary from the
  [ninja-build releases](https://github.com/ninja-build/ninja/releases) (`ninja-mac.zip`).
- **Python:** `pip3 install cmake ninja` (both ship as wheels).
- **MacPorts:** `sudo port install cmake ninja`.

Qt 6.5+ comes from the [Qt online installer](https://www.qt.io/download-qt-installer)
(or `brew install qt`; then set `QT_ROOT_DIR=$(brew --prefix qt)`).

```bash
export QT_ROOT_DIR=~/Qt/6.5.3/macos
cmake --preset macos && cmake --build --preset macos && ctest --preset macos
cmake --preset macos-release && cmake --build --preset macos-release
PATH="$QT_ROOT_DIR/bin:$PATH" packaging/macos/package.sh build/macos-release build/macos-package
```
