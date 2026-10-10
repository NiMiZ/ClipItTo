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
