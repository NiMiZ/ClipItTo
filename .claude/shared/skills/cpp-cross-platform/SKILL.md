---
name: cpp-cross-platform
description: Rules for C++ code that must build and run on both Linux and Windows. Use when touching platform-specific code, #ifdefs, file paths, threading, CMake files, or packaging.
---

# Cross-platform C++ rules

## Structure
- Keep OS code behind an interface (e.g. `ClipboardBackend`, `MonitorBackend`) with `*_linux.cpp` and `*_win.cpp` implementations chosen in CMake. Don't scatter `#ifdef`s through app logic.
- Any change to one platform's implementation needs the matching change on the other platform in the same task. If a feature can't exist on one side, return an explicit "unsupported" error; never silently do nothing.
- Platform guards: `#if defined(_WIN32)` / `#elif defined(__linux__)` / `#else #error "unsupported platform"`.

## Windows
- Use the wide (`W`) Win32 APIs. Convert UTF-16 <-> UTF-8 at the backend boundary; app code uses UTF-8.
- Define `WIN32_LEAN_AND_MEAN` and `NOMINMAX` before including `<windows.h>`.
- Wrap every HANDLE, HWND registration, and physical-monitor handle in an RAII type.
- MSVC flags: `/W4 /permissive- /utf-8`.

## Linux
- Check every syscall return value; report failures with `errno` / `strerror`.
- Never assume X11. Detect Wayland (`WAYLAND_DISPLAY`) and either support it or report clearly that a feature is unavailable.
- GCC/Clang flags: `-Wall -Wextra -Wpedantic`.

## General
- Paths: `std::filesystem::path` only, no string concatenation with `/` or `\\`.
- Config/data locations: `QStandardPaths` (or XDG dirs on Linux, `%APPDATA%`/`%LOCALAPPDATA%` on Windows). Never hardcode.
- Never block the GUI thread: clipboard reads of large data, I2C/DDC calls, disk I/O all go to a worker thread, with results posted back via signals/queued calls.
- OS callbacks (window procedures, X11/Wayland event handlers) must not touch widgets directly; post to the Qt event loop.

## Building and verifying
- Use CMake presets (`cmake --preset <name>`, `cmake --build --preset <name>`). If `CMakePresets.json` is missing, propose adding one with `linux-debug`, `linux-release`, `windows-debug`, `windows-release`.
- After changing platform code, build every configuration that has a toolchain available. If only one platform can be built here, say explicitly which side is unverified.
