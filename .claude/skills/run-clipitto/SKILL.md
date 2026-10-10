---
name: run-clipitto
description: Build, run, test, drive and screenshot the ClipItTo clipboard manager (Qt/C++ daemon, popup UI and CLI) headlessly on Linux. Use when asked to run, start, launch, try, smoke-test, demo or take a screenshot of ClipItTo or its popup, or to check a change in the real app rather than in unit tests.
---

# Run ClipItTo

ClipItTo is a background daemon (`clipitto-daemon`: clipboard capture, SQLite history and
the Qt popup) plus a CLI (`clipitto`) that controls it over D-Bus. The agent path drives the
**real daemon on a private Xvfb display** with `driver.py`. Another "app" copies text with
`xclip`, the popup is opened through the real CLI, keys are typed with `xdotool`, and
screenshots are grabbed from the X server. Each driver command is one step, and state lives
in `/tmp/clipitto-driver`.

All paths below are relative to the repository root.

## Prerequisites

Without root, on Ubuntu 24.04, run this once. It unpacks Qt 6, SQLite, toml++, the X11 dev
packages, Xvfb, xdotool and xclip with `apt-get download` + `dpkg -x`:

```bash
sh .claude/skills/run-clipitto/setup-noroot.sh /tmp/clipitto-deps
. /tmp/clipitto-deps/env.sh          # in EVERY new shell: build and runtime need it
```

The driver also needs `python3` with Pillow (XCB support), `dbus-daemon`, and the system's
`xkbcomp`/`xkb-data` (all present on stock Ubuntu desktop images). With root, install the
packages from the README's Building section and skip `env.sh`.

## Build

```bash
cmake --preset debug
cmake --build --preset debug         # ~40 s; binaries in build/debug/src/{daemon,cli}
```

## Run (agent path): the driver

Scripted end-to-end flow with checks. It starts and stops everything itself and exits
non-zero on failure:

```bash
python3 .claude/skills/run-clipitto/driver.py smoke
```

It copies three texts from "another app", checks they were captured, opens the popup with
`clipitto --toggle`, searches, presses Enter and checks the clipboard. It then assigns a
group through the real Ctrl+G dialog and opens the group tree. Screenshots go to
`/tmp/clipitto-driver/screenshots/01-popup.png`, `02-search.png` and `03-groups.png`. Look at them.

Step by step (each command returns when its step is done):

```bash
D=.claude/skills/run-clipitto/driver.py
python3 $D start                     # Xvfb + private D-Bus bus + clipitto-daemon (Qt xcb)
python3 $D copy "ssh admin@db-01"    # another app copies -> daemon captures it
python3 $D cli --toggle              # the real CLI: --toggle --show --hide --status --reload --quit
python3 $D type "ssh"                # type into the focused window
python3 $D ss after-search           # -> /tmp/clipitto-driver/screenshots/after-search.png
python3 $D key Return                # xdotool key names: Down Up Right Return Escape ctrl+g ctrl+comma ...
python3 $D clipboard                 # what is on the clipboard now
python3 $D status                    # daemon status as JSON (backend, items, visible, ...)
python3 $D stop                      # quit daemon, stop bus + Xvfb (logs/screenshots kept)
```

Useful keys in the popup:
- `Down`: from the search box to the first row.
- `Right`: in the empty search box, opens the Groups tree; on a group, opens it.
- `ctrl+g` / `ctrl+e` / `ctrl+s`: group / note / favorite on a copy.
- `Delete`: removes a copy.
- `ctrl+comma`: Settings.
- `Escape`: closes the popup.

The daemon's config is `/tmp/clipitto-driver/config/clipitto/config.toml`; edit it, then
`python3 $D cli --reload`. Logs are in `/tmp/clipitto-driver/logs/{daemon,xvfb}.log`.

`CLIPITTO_BUILD` points the driver at another build dir (default `build/debug`), and
`CLIPITTO_DRIVER_DIR` moves the session dir.

## Test (direct invocation)

Most changes are in `src/core` or `src/ui`, and the Qt tests drive that code in-process
(offscreen) in seconds:

```bash
ctest --preset debug                                   # all 20 suites, ~5 s
ctest --preset debug -R tst_popup                      # one suite
QT_QPA_PLATFORM=offscreen build/debug/tests/tst_popup keyboardWalksTheGroupTree   # one test
```

The GNOME Shell extension is tested under gjs with stub shell modules
(`gnome_extension` in ctest), and D-Bus behavior on a private bus (`tst_dbus`,
`dbus_activation`).

## Run (human path)

`build/debug/src/daemon/clipitto-daemon &` then `build/debug/src/cli/clipitto --toggle` opens
the popup on your desktop. Under WSLg it opens on the Wayland display, where capture is
degraded without the GNOME extension. For daily use, install it as the README describes.

## Gotchas

- **The host session leaks in.** In WSLg, or any GUI shell, `WAYLAND_DISPLAY` and
  `XDG_CURRENT_DESKTOP` are set. Qt then picks Wayland and the daemon reports
  `backend_degraded: true`. The driver strips them and sets `QT_QPA_PLATFORM=xcb` and
  `XDG_SESSION_TYPE=x11`; do the same for any manual run on Xvfb.
- **No X socket file under WSLg.** `/tmp/.X11-unix` belongs to WSLg, so Xvfb logs
  `failed to bind listener` and listens on the abstract socket only. That's harmless.
  Check readiness by connecting (`xdotool getdisplaygeometry`), never by testing for
  `/tmp/.X11-unix/X99`.
- **`Could not resolve keysym XF86...` warnings** from xkbcomp at Xvfb start are harmless.
- **A copy needs a live owner.** X clipboard data is served by the app that copied it. The
  driver leaves `xclip -quiet` running in the background; a short-lived `xclip` can vanish
  before the daemon reads the text.
- **Search is debounced (100 ms).** Wait after typing before pressing Enter or taking a
  screenshot (`driver.py type` does).
- **Private bus, on purpose.** The driver's D-Bus bus has no service directories, so an
  installed ClipItTo can never be auto-started instead of the build. Without the driver,
  the CLI can't reach the daemon unless both share `DBUS_SESSION_BUS_ADDRESS`.
- **The Settings shortcut field is read-only under the driver.** The session isn't GNOME,
  and changing the global shortcut is only implemented for GNOME (gsettings) and Windows.
  GNOME/Wayland paths (the extension, focus, the gsettings shortcut) can't run here; use the
  gjs and fake-gsettings tests.
- **No window manager is needed.** Xvfb gives the popup keyboard focus, and it opens near
  the pointer (screen center).
- **`pgrep -f clipitto` matches your own shell command.** Check leftovers with
  `pgrep -x clipitto-daemon` / `pgrep -x Xvfb`.

## Troubleshooting

- `Could not find a package configuration file provided by "Qt6"`: `env.sh` isn't sourced
  in this shell, or the deps dir was wiped (e.g. after a WSL restart). Re-run
  `setup-noroot.sh`.
- `rcc: error while loading shared libraries: libQt6Core.so.6` or
  `Could not find the Qt platform plugin "offscreen"`: same cause. `env.sh` sets
  `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH`, which builds and test runs both need.
- `Does not match the generator used previously: Unix Makefiles`: that build dir was
  configured without the preset. Use `cmake --preset debug` (Ninja) or another dir.
- `ninja: error: '.../libsqlite3.so', needed by ..., missing`: a dangling dev symlink in a
  hand-made sysroot. `setup-noroot.sh` repoints those at the system libraries.
- `driver: already running on :99`: a previous session is still up. Run `driver.py stop`.
  A failed `start` cleans up after itself.
