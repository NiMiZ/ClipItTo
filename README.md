# ClipItTo
Clipboard manager with history, groups and favorites for Linux and Windows

Press a shortcut, type a few letters, press Enter: the clip is back on your clipboard.
Pin up to 10 clips to the top, star clips into Favorites, sort clips into your own groups
(none of these are ever pruned), and give clips a readable title with a note.

## Download for Ubuntu 24.04+

Install the latest release with one command (it downloads the `.deb`, checks its SHA-256
against the release and installs it with apt, which pulls in the Qt libraries it needs):

```bash
curl -fsSL https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.sh | sh
```

Or get `clipitto_<version>_amd64.deb` from the
[latest release](https://github.com/NiMiZ/ClipItTo/releases/latest) and install it yourself:
`sudo apt install ./clipitto_0.2.0_amd64.deb`. [install.sh](install.sh) also takes
`--version X.Y.Z` and `--download-only DIR`.

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
sudo apt install ./build/deb/clipitto_0.2.0_amd64.deb
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
| `↓` / `↑` | move the selection (it starts on the latest copy: `↓` next copy, `↑` the Groups row); `↑` on the top row returns to the search bar |
| `Enter` / click | copy the entry back to the clipboard and close (on a group: open/close it) |
| `→` / `←` | open / close a group; `→` in the empty search box opens **Groups** |
| `Ctrl+S` / `Ctrl+P` | star / pin to the top (again to undo) |
| `Ctrl+G` / `Ctrl+E` | assign group (on a group: rename) / edit note |
| `Alt+↑` / `Alt+↓` | move a group, or a pinned copy, up or down |
| `Del` | delete the entry (on a group: delete the group, its copies are kept) |
| right-click or `Menu` | the same actions in a menu, plus new group / sub-group on groups |
| `Ctrl+,` or ⚙ | settings |
| `Esc` | close |

**Groups** sit collapsed right under the search box. Open them to browse your groups as a
tree: each group lists its sub-groups, then its copies. Put a copy in a group with `Ctrl+G`
and a path such as `Work/Servers` (missing groups are created). Groups appear in the order
you give them (`Alt+↑/↓`, or Move up/down in Settings); new ones go last.

**Stars** (`Ctrl+S`): a starred copy that is in no group is kept in **★ Favorites**, which is
always the first group (up to 10 copies; Settings allows up to 20). A starred copy in a group
is listed first in that group, up to 10 per group. Moving a starred copy into a group, or out
of one, takes its star along; ClipItTo tells you when the target is full.

**Pinned** (`Ctrl+P`): up to 10 copies listed at the top, above *Recent*, in the order you
pin them (`Alt+↑/↓` reorders them). Pinned, starred and grouped copies are never pruned.

**Recent** is your copy history: each text once, the one you last copied or pasted first
(pasting a copy from ClipItTo counts as copying it), starred and grouped copies included.
Only pinned copies are left out, as they are listed right above.

**Settings** (⚙): theme (system / light / dark), how many copies are listed, how many text
lines each copy shows, how many Favorites to keep, where the popup opens (near the pointer, centered, a corner, or where it was last: drag it by its name), whether a copy you choose becomes the latest, the shortcut
(default Ctrl+Alt+V; e.g. Ctrl+` works too; on Windows ClipItTo registers it, on GNOME it
becomes the GNOME keyboard shortcut), history encryption, and group management (new,
sub-group, rename/move, delete, move up/down; saved immediately). Saving keeps your own comments and
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
Choosing a copy also pastes it into the window you came from; turn that off in Settings
(*After choosing a copy: Paste it into the window I came from*) or with `paste_on_enter = false` to only copy it. On GNOME Wayland
pasting needs the bundled extension.

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

## Privacy

Your clipboard history stays on your computer. ClipItTo makes no network connections: no
cloud sync, no accounts, no telemetry, no update checks. Copies, notes, groups and settings
are stored only in local files under your user account (`~/.local/share/clipitto/` and
`~/.config/clipitto/` on Linux, `%LOCALAPPDATA%\clipitto\` and `%APPDATA%\clipitto\` on
Windows), readable by your account only; turn on [encryption](#encrypted-history) to
protect them at rest as well. The daemon, the CLI and the GNOME extension talk to each
other over your session's D-Bus or a local socket, never over the network. Logs never
contain clipboard contents. To erase everything, quit ClipItTo and delete those folders.

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

**Install:** in PowerShell, run

```powershell
irm https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.ps1 | iex
```

It downloads the installer, checks its SHA-256, installs ClipItTo for your account with
"Start ClipItTo when I sign in", and starts it. ([install.ps1](install.ps1) also takes
`-Version X.Y.Z` and `-DownloadOnly DIR` when run as a file.)

Or download `ClipItTo-<version>-setup.exe` from the
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

**Build from source:** Visual Studio 2022 or newer, or its Build Tools, with the *Desktop
development with C++* workload; Qt 6.5+ for MSVC; and [vcpkg](https://vcpkg.io), which builds
toml++ from [vcpkg.json](vcpkg.json). The script finds them (Qt and vcpkg in `D:\` or `C:\`, or
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

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds and tests both platforms and
builds the Debian package.
