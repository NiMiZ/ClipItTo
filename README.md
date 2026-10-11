# ClipItTo
Clipboard manager with history, groups and favorites for Linux and Windows

Press a shortcut, type a few letters, press Enter: the clip is back on your clipboard.
Favorites and grouped clips are never pruned and are listed first; notes give clips a
readable title.

## Download for Ubuntu 24.04+

Get `clipitto_<version>_amd64.deb` from the
[latest release](https://github.com/NiMiZ/ClipItTo/releases/latest) and install it (apt
pulls in the Qt libraries it needs):

```bash
sudo apt install ./clipitto_0.1.0_amd64.deb
```

Then, once per user, **log out and back in** (so GNOME Shell sees the extension) and run
`clipitto-setup`; see [Installing](#installing-ubuntu-2404) for what it does. Press
**Ctrl+Alt+V** to open the popup. To remove it: `sudo apt remove clipitto`.

## Quick start on Ubuntu 24.04+ (from source, no install)

```bash
# 1. Dependencies
sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-wayland libgl-dev \
  libsqlite3-dev libtomlplusplus-dev libx11-dev libxtst-dev libglib2.0-bin qtkeychain-qt6-dev wl-clipboard

# 2. Build and test
git clone https://github.com/NiMiZ/ClipItTo.git && cd ClipItTo
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release --output-on-failure

# 3. Run the daemon and open the popup
build/release/src/daemon/clipitto-daemon &
build/release/src/cli/clipitto --toggle     # bind this command to a shortcut to use it daily
build/release/src/cli/clipitto --status     # which capture backend is active
```

On **GNOME Wayland** (the Ubuntu default) the daemon can only see copies made while its own
window has focus until the GNOME extension is installed (`backend_degraded: true` in
`--status`). Install the extension from the source tree, log out and back in, then enable it:

```bash
gnome-extensions pack --force gnome-extension/clipitto@nimiz.github.io
gnome-extensions install --force clipitto@nimiz.github.io.shell-extension.zip
# log out and back in, then:
gnome-extensions enable clipitto@nimiz.github.io
build/release/src/cli/clipitto --status     # backend: gnome-ext
```

On **Ubuntu on Xorg** no extension is needed. For daily use, install it properly as below.

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
| `Enter` / click | copy the entry back to the clipboard and close (on a group: open/close it) |
| `→` / `←` | open / close a group; `→` in the empty search box opens **Groups** |
| `Ctrl+S` / `Ctrl+G` / `Ctrl+E` | favorite / assign group (on a group: rename) / edit note |
| `Del` | delete the entry (on a group: delete the group, its copies are kept) |
| right-click or `Menu` | the same actions in a menu, plus new group / sub-group on groups |
| `Ctrl+,` or ⚙ | settings |
| `Esc` | close |

**Groups** sit collapsed right under the search box. Open them to browse your groups as a
tree: each group lists its sub-groups, then its copies. Put a copy in a group with `Ctrl+G`
and a path such as `Work/Servers` (missing groups are created). Grouped and favorite copies
are never pruned and are also listed under *Saved*.

**Settings** (⚙): theme (system / light / dark), how many copies are listed, how many text
lines each copy shows, where the popup opens (near the pointer or centered), the shortcut
(default Ctrl+Alt+V; e.g. Ctrl+` works too; on Windows ClipItTo registers it, on GNOME it
becomes the GNOME keyboard shortcut), history encryption, and group management (new,
sub-group, rename/move, delete; saved immediately). Saving keeps your own comments and
layout in `config.toml`.

### Encrypted history

Settings › *Encrypt the history on disk* encrypts `history.db` (copies, notes, groups, the
search index) with 256-bit ChaCha20-Poly1305 via the bundled
[SQLite3 Multiple Ciphers](https://github.com/utelle/SQLite3MultipleCiphers). The random key
is kept in the system keyring (GNOME Keyring / KWallet through Secret Service, or the
Windows Credential Manager), so there is no password to type; without a reachable keyring
the option says why it is unavailable. Turning it on or off converts the history once, in
the background. It protects the file at rest (backups, other accounts, a stolen disk
without full-disk encryption), not against programs running as you, which can read the
clipboard and the keyring anyway. Copies made before turning it on may still be
recoverable from free disk space (old journal blocks); only full-disk encryption covers
that. If the keyring entry is lost, the history cannot be read. History files are created
readable by your account only.

Measured cost (`build/debug/tests/bench_encryption`, debug build, 1000 copies):

| | plain | encrypted |
|---|---|---|
| open + first read | 0.6 ms | 27 ms |
| store one copy | 0.37 ms | 0.88 ms |
| search | 4.0 ms | 4.2 ms |
| one-time conversion | | ~50 ms |

The build downloads the SQLite3 Multiple Ciphers amalgamation (pinned version and SHA-256);
offline builds can set `FETCHCONTENT_SOURCE_DIR_SQLITE3MC` to an unpacked copy, or
configure with `-DCLIPITTO_ENCRYPTION=OFF` to use the system SQLite.

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
  libtomlplusplus-dev libx11-dev libxtst-dev libglib2.0-bin qtkeychain-qt6-dev
cmake --preset debug
cmake --build --preset debug
ctest --preset debug     # gjs and dbus-run-session enable the extension and D-Bus tests
```

Presets: `debug`, `release`, `asan` (address/UB sanitizers, warnings as errors), `deb`
(installs under `/usr`, for `cpack`), `windows`. Search benchmark: `build/debug/tests/bench_search`.

The daemon runs as the `clipitto.service` systemd user unit and is also started on demand
through D-Bus (`io.github.nimiz.ClipItTo`, see [the interface](src/ipc/io.github.nimiz.ClipItTo.xml)).

## Windows 10/11

**Install:** download `ClipItTo-<version>-setup.exe` from the
[latest release](https://github.com/NiMiZ/ClipItTo/releases/latest) (builds of every commit are
in the [CI runs](https://github.com/NiMiZ/ClipItTo/actions): artifact `clipitto-windows-installer`,
a portable folder in `clipitto-windows-portable`), run it, and keep "Start ClipItTo when
I sign in" ticked. No admin rights are needed.

**Use:** press **Ctrl+Alt+V** anywhere (Win+V is Windows' own clipboard history). The keys in the
popup are the same as on Linux. ClipItTo lives in the notification area: click its icon to
open the popup, right-click it to reload settings or quit.

Settings are in `%APPDATA%\clipitto\config.toml` (same keys as
[config.example.toml](data/config.example.toml); `hotkey = "Ctrl+Shift+V"` changes the
shortcut), history in `%LOCALAPPDATA%\clipitto\history.db`. `clipitto.exe --status`,
`--reload` and `--quit` (in the install folder's `bin`) work as on Linux.

**Build from source:** Visual Studio 2022 Build Tools (C++ workload), Qt 6.5+ for MSVC and
[vcpkg](https://vcpkg.io), which builds SQLite and toml++ from [vcpkg.json](vcpkg.json).
From a *Developer PowerShell for VS*:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"; $env:QT_ROOT_DIR = "C:\Qt\6.5.3\msvc2019_64"
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
cmake --install build/windows --prefix $PWD\stage   # runnable folder (absolute path required)
iscc /DStageDir=$PWD\stage packaging\windows\clipitto.iss   # optional installer (Inno Setup 6)
```

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds and tests both platforms and
builds the Debian package.
