# ClipItTo
Clipboard manager with history, groups and favorites for Linux

Press a shortcut, type a few letters, press Enter: the clip is back on your clipboard.
Favorites and grouped clips are never pruned and are listed first; notes give clips a
readable title.

## Installing (Ubuntu 24.04+)

Build the package (see [Building](#building) for the dependencies) and install it:

```bash
cmake --preset deb && cmake --build --preset deb
(cd build/deb && cpack)
sudo apt install ./build/deb/clipitto_0.1.0_amd64.deb
```

Then, once per user, **log out and back in** (so GNOME Shell sees the extension) and run:

```bash
clipitto-setup            # starts the daemon with your session, enables the GNOME extension,
                          # binds Ctrl+Alt+V; pass another binding, e.g. clipitto-setup '<Super>v'
```

Without root, install into `~/.local` instead (needs `~/.local/bin` on `PATH`):

```bash
cmake --preset release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build --preset release && cmake --install build/release
clipitto-setup
```

`clipitto-shortcut [BINDING]` changes the shortcut later; `clipitto-shortcut --remove` removes it.

## Using it

| Key | Action |
|---|---|
| type | filter history (note, group and text) |
| `↓` / `↑` | move between the search bar and the list |
| `Enter` / click | copy the entry back to the clipboard and close |
| `Ctrl+S` / `Ctrl+G` / `Ctrl+E` | favorite / assign group / edit note |
| `Del` | delete the entry |
| right-click or `Menu` | the same actions in a menu |
| `Esc` | close |

```bash
clipitto --toggle | --show | --hide     # what the shortcut runs
clipitto --status                       # backend, item counts, extension state
clipitto --reload                       # after editing ~/.config/clipitto/config.toml
```

Settings: see [config.example.toml](data/config.example.toml) (installed under
`/usr/share/doc/clipitto/`). History lives in `~/.local/share/clipitto/history.db`.
With `paste_on_enter = true`, Enter also pastes into the window you came from.

### How capture works per session

| Session | Backend |
|---|---|
| GNOME Wayland + the bundled extension | `gnome-ext` |
| Other Wayland compositors with data-control (wlroots, KDE) + `wl-clipboard` | `wl-paste` |
| X11, Windows | `qt-clipboard` |
| Wayland without either | `qt-clipboard`, degraded: only copies made while the popup has focus |

GNOME's Wayland session lets no regular app watch the clipboard or take focus, so ClipItTo
ships a small [GNOME Shell extension](gnome-extension/clipitto@nimiz.github.io) that reports
clipboard changes to the daemon and lets the popup take focus and paste. The daemon notices
the extension coming and going. The extension can also own the shortcut itself, which is the
most reliable option on Wayland (then remove the custom shortcut with `clipitto-shortcut --remove`):

```bash
gsettings --schemadir /usr/share/gnome-shell/extensions/clipitto@nimiz.github.io/schemas \
  set org.gnome.shell.extensions.clipitto toggle-shortcut "['<Control><Alt>v']"
```

## Building

Requires Ubuntu 24.04+ (or equivalent): Qt 6.4+, SQLite 3.35+, toml++ 3.

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev libgl-dev libsqlite3-dev \
  libtomlplusplus-dev libx11-dev libxtst-dev libglib2.0-bin
cmake --preset debug
cmake --build --preset debug
ctest --preset debug     # gjs and dbus-run-session enable the extension and D-Bus tests
```

Presets: `debug`, `release`, `asan` (address/UB sanitizers, warnings as errors), `deb`
(installs under `/usr`, for `cpack`), `windows`. Search benchmark: `build/debug/tests/bench_search`.

The daemon runs as the `clipitto.service` systemd user unit and is also started on demand
through D-Bus (`io.github.nimiz.ClipItTo`, see [the interface](src/ipc/io.github.nimiz.ClipItTo.xml)).

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

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds and tests both platforms and
builds the Debian package.
