# ClipItTo
Clipboard manager with history, groups and favorites for Linux

## Building

Requires Ubuntu 24.04+ (or equivalent): Qt 6.4+, SQLite 3.35+, toml++ 3.

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev libgl-dev libsqlite3-dev libtomlplusplus-dev
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (address/UB sanitizers, warnings as errors), `windows`.

### Windows (work in progress)

The core, capture and daemon libraries and their tests build natively with MSVC; the
Windows entry point (global hotkey, named-pipe CLI, installer) is a later milestone.
Needs Qt 6.5+ for MSVC and [vcpkg](https://vcpkg.io) (installs SQLite and toml++ from
[vcpkg.json](vcpkg.json)). From a *Developer PowerShell for VS*:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"; $env:QT_ROOT_DIR = "C:\Qt\6.5.3\msvc2019_64"
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds and tests both platforms.
Search benchmark: `build/debug/tests/bench_search`.

## Running

```bash
build/debug/src/daemon/clipitto-daemon &      # background daemon (later: systemd --user unit)
build/debug/src/cli/clipitto --status          # what the daemon is doing
build/debug/src/cli/clipitto --toggle          # bind this to a global shortcut
```

### GNOME on Wayland: the companion extension

GNOME's Wayland session lets no regular app watch the clipboard or take focus, so ClipItTo
ships a small GNOME Shell extension ([gnome-extension/](gnome-extension/clipitto@nimiz.github.io))
that reports clipboard changes to the daemon and lets the popup take focus (and paste, with
`paste_on_enter = true`). Install it once, then log out and back in:

```bash
gnome-extensions pack --force gnome-extension/clipitto@nimiz.github.io
gnome-extensions install --force clipitto@nimiz.github.io.shell-extension.zip
# after logging back in:
gnome-extensions enable clipitto@nimiz.github.io
```

The daemon notices the extension coming and going; `clipitto --status` shows the active
capture backend:

| Session | Backend |
|---|---|
| GNOME Wayland + extension | `gnome-ext` |
| Other Wayland compositors with data-control (wlroots, KDE) + `wl-clipboard` | `wl-paste` |
| X11, Windows | `qt-clipboard` |
| Wayland without either | `qt-clipboard`, degraded: only copies made while the popup has focus |

The extension can also own the shortcut itself (most reliable on Wayland):
`gsettings --schemadir ~/.local/share/gnome-shell/extensions/clipitto@nimiz.github.io/schemas set org.gnome.shell.extensions.clipitto toggle-shortcut "['<Control><Alt>v']"`.

### Popup keys

| Key | Action |
|---|---|
| type | filter history (note, group and text) |
| `↓` / `↑` | move between the search bar and the list |
| `Enter` / click | copy the entry back to the clipboard and close |
| `Ctrl+S` / `Ctrl+G` / `Ctrl+E` | favorite / assign group / edit note |
| `Del` | delete the entry |
| right-click or `Menu` | the same actions in a menu |
| `Esc` | close |

Favorites and grouped entries are never pruned and are listed first.

The CLI talks to the daemon over D-Bus (`io.github.nimiz.ClipItTo`, see
[the interface](src/ipc/io.github.nimiz.ClipItTo.xml)). `kill -HUP` or `clipitto --reload`
re-reads `~/.config/clipitto/config.toml`; history lives in `~/.local/share/clipitto/history.db`.
