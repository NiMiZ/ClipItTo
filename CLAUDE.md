# Clipboard History Manager

C++20, Qt 6 (Widgets), CMake. Targets Windows 10+ and Linux (X11 and Wayland).

## Layout
- `src/core/` platform-independent history logic (dedupe, storage, search). No OS headers here.
- `src/platform/` `ClipboardBackend` interface plus `clipboard_win.cpp`, `clipboard_x11.cpp`, `clipboard_wayland.cpp`.
- `src/ui/` Qt widgets: tray icon, history popup, settings.
- `tests/` unit tests against a fake backend.

## Build
- `cmake --preset debug && cmake --build --preset debug`
- Windows: `cmake --preset windows && cmake --build --preset windows`
- Tests: `ctest --preset <preset> --output-on-failure`

## Rules
- Never log clipboard contents.
- Content marked by password managers is never stored in clear: it is skipped, or, once a master password is set, kept only as a sealed password copy (see the clipboard-backends skill). Backends read its text only while password copies are kept.
- Use the build-runner agent for builds, and the privacy-reviewer agent after touching capture, storage, or logging.

## Working notes (read when touching these areas)
- `.claude/notes/display-and-search.md` — popup, Recent rules, search
- `.claude/notes/settings-groups-backend.md` — storage, groups, duplicates, config, backup, CI facts
- `.claude/notes/passwords.md` — password copies: intent and implementation
