# ClipItTo
Clipboard manager with history, groups and favorites for Linux, Windows and macOS

Press a shortcut, type a few letters, press Enter: the clip (text or picture) is back on
your clipboard.
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
`sudo apt install ./clipitto_0.6.0_amd64.deb`. [install.sh](install.sh) also takes
`--version X.Y.Z` and `--download-only DIR`.

Then, once per user, **log out and back in** (so GNOME Shell sees the extension) and run
`clipitto-setup`; see [Installing](#installing-ubuntu-2404) for what it does. Press
**Ctrl+Alt+V** to open the popup. To remove it: `sudo apt remove clipitto`.

## Download for other Linux distributions

Two downloads carry everything inside (Qt and the other libraries), so nothing else needs
installing. Both are on the [latest release](https://github.com/NiMiZ/ClipItTo/releases/latest).

**AppImage** (`ClipItTo-<version>-x86_64.AppImage`): one file for most distributions from 2024
on (glibc 2.39+: Ubuntu 24.04, Fedora 40, Debian 13, Arch, ...).

```bash
chmod +x ClipItTo-*-x86_64.AppImage
./ClipItTo-*-x86_64.AppImage --install    # start with the session, menu entry, GNOME extension, shortcut
```

`--install` sets it up for your account, pointing at the AppImage where it is (keep it
there); on GNOME, log out and back in and run `--install` once more to enable the extension.
The AppImage also takes every `clipitto` option (`--toggle`, `--status`, `--lock`, ...).
`--uninstall` undoes the setup and keeps your history. Without FUSE (some containers), set
`APPIMAGE_EXTRACT_AND_RUN=1`.

**Snap** (`clipitto_<version>_amd64.snap`), strictly confined:

```bash
sudo snap install --dangerous ./clipitto_*_amd64.snap
```

`--dangerous` is needed while it isn't in the Snap Store. Then run `clipitto.setup` once
(ClipItTo starts with your session from then on), start it now with
`clipitto.clipitto-daemon &`, and bind `clipitto --toggle` to a shortcut. On GNOME, the
extension is not part of the snap; install it from the `.deb` or the AppImage (`--install`).

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
sudo apt install ./build/deb/clipitto_0.6.0_amd64.deb
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
| `Del` | delete the entry (on a group: delete the group, its copies are kept); in the search box too, when the cursor is at the end |
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

**Recent** is your copy history: each text once, the one you last copied first, starred and
grouped copies included. Only pinned copies are left out, as they are listed right above.
Pasting a copy from ClipItTo moves it to the top of Recent (`latest_after_paste`), except starred,
pinned and grouped copies, which stay where they are.

- *Delete* on a starred or grouped copy in Recent only takes it off Recent: it stays in its
  group (and comes back to Recent when you copy it again). In a group, or on an unfiled copy,
  Delete deletes it.
- Right-click a group (or the *Favorites* row) › *Keep out of Recent*: its copies, and those of
  its sub-groups, don't appear in Recent. The Passwords group has this as the *Also show them in
  Recent* setting.

**Backup** (Settings › Backup): *Export…* writes your copies (with notes, stars, pins, groups and
pictures) and/or your settings to one JSON file; *Import…* adds a file's contents to what is here
(copies already in the history are left alone). Tick *Copies* and/or *Settings* when exporting;
when you import a file that has both, ClipItTo asks which to import. With a master password set,
both ask for it first. Password copies are encrypted in the file with your master password
(Argon2id + XChaCha20-Poly1305), not with anything of this computer, so the file opens on another
PC with that password; importing re-seals them with that PC's own master password (set one there
first). Everything else in the file is plain text: keep it private. Imported settings leave this
computer's shortcut, backend, history encryption and Windows Hello choices as they are.

**Clear** (the button next to ⚙) empties Recent: unfiled copies are deleted, starred and grouped
ones only leave Recent and stay in their groups; pinned ones stay pinned. Its menu (the arrow)
also offers *Clear everything, also in groups*, which deletes those as well and leaves the groups
empty. Both ask first and can't be undone.

**Pictures:** copied images (screenshots, "Copy Image" in a browser, ...) are kept too, shown
as a thumbnail with their size; hover a row for a larger preview, and choose it to put the
picture back on the clipboard. A copy that has both text and a picture is kept as text.
Pictures have their own limit, *Pictures kept in history* in Settings (`max_images`, default
100 unsaved pictures; 0 records none), and pictures larger than 50 MB (`max_image_bytes`) are
skipped. They are stored, and encrypted when that's on, in the same history file. Search
finds them by note, group or "image".

**Settings** (⚙): theme (system / light / dark), how many copies are listed, how many text
lines each copy shows, how many copies and pictures the history keeps, how many Favorites to keep, where the popup opens (near the pointer, centered, a corner, or where it was last: drag it by its name), where the selection starts when it opens and after a search (*Start on*: the first pinned copy, the first copy in a group, or the first unsaved one), whether a copy you choose becomes the latest, the shortcut
(default Ctrl+Alt+V; e.g. Ctrl+` works too; on Windows ClipItTo registers it, on GNOME it
becomes the GNOME keyboard shortcut, set again at every start; on KDE, Sway and other
desktops bind `clipitto --toggle` yourself, and `clipitto --status` reminds you), history encryption, group management (new,
sub-group, rename/move, delete, move up/down; saved immediately), and
[password copies](#password-copies) (the master password, Windows Hello, the search keyword). Saving keeps your own comments and
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

### Password copies

Password managers (KeePassXC, Bitwarden, KeePass, 1Password, ...) mark what they copy as
secret. Without a master password ClipItTo skips those copies, as before; the popup says once
how to keep them. Once you set a master password in Settings › *Passwords*:

- **Kept encrypted.** A marked copy is encrypted the moment it arrives (X25519 + XChaCha20-Poly1305,
  via the built-in [Monocypher](https://monocypher.org)) and only that is stored. It works while
  locked: copies are encrypted to a public key, and only unlocking can open them.
- **Hidden.** It is listed as `●●●●` with a padlock, its note set to the app it came from
  (Windows, X11, GNOME; not with wl-paste). Ordinary search finds it by note or group only.
- **Unlocked with the master password.** Choosing one (Enter), *Show password* (10 s in its row)
  and *Not a password* ask for it first. An unlock lasts 5 minutes (`password_unlock_minutes`);
  `clipitto --lock` or *Lock passwords* ends it. Unlocking takes about 0.1 s (Argon2id, 64 MiB).
  On Windows, *Unlock with Windows Hello too* lets your face, fingerprint or PIN do it.
- **Pasted marked, then cleared.** A password pasted back carries the password-manager markers,
  so other clipboard managers and Windows' clipboard history skip it too (not through the GNOME
  extension, which can put only one format on the clipboard). It leaves the clipboard after
  30 s (`password_clear_seconds`; 0 = never) unless you copied something else meanwhile.
- **Searched with a keyword.** Type `p[] ` (the keyword and a space; Settings can change it):
  what follows shows as asterisks and searches inside your passwords while unlocked, or for a
  whole password while locked. Locked searching needs the system keyring, which keeps the key
  for it; the history file alone can't be used to test guesses, and with the keyring each guess
  still costs a memory-hard hash (Argon2id, 16 MiB).
- Right-click *Mark as password* turns any copy into one (its text is wiped from the file);
  *Put new password copies in the "Passwords" group* files them there (off by default). With
  that on, *Also show them in Recent* chooses whether they also appear in the Recent list
  (as `●●●●`) or only in the group and the `p[]` search.
- *After copying a password, ask for a name* (off by default): when a **new** password is
  copied, ClipItTo opens with a box to name it. The app it came from is suggested as grey
  text that goes away when you type; leave the box empty (or cancel) to keep that name. The
  window closes again afterwards if it wasn't open. (On Windows the window may not take the
  focus when it opens by itself.)

The master password is never stored or shown; only a key derived from it unlocks. Changing it
also changes the key every password copy is sealed with. **If you forget it, the password
copies can't be recovered**: *Remove…* deletes them and starts over.

This protects them in the history file and from people using your computer while they are
locked. It doesn't protect them from programs running as you: those can read the clipboard
when a password is copied, and on GNOME a copied password travels from the shell extension to
the daemon over your session's D-Bus. Passwords don't lock by themselves when the screen locks
yet; use `clipitto --lock` (e.g. from your screen-lock hook) or a short unlock time.

```bash
clipitto --toggle | --show | --hide     # what the shortcut runs
clipitto --lock                         # lock the password copies now
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
contain clipboard contents. Copies that password managers mark as secret are skipped, or kept
only encrypted once you set a [master password](#password-copies). To erase everything, quit ClipItTo and delete those folders.

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

## macOS 11+

Apple Silicon and Intel in one app. **New: CI builds and tests it on real Macs (the unit tests,
and the app run against the real pasteboard), but the global shortcut and auto-paste still
need hands-on testing on a real machine.**

**Install:** download `ClipItTo-<version>-macos.dmg` from the
[latest release](https://github.com/NiMiZ/ClipItTo/releases/latest), open it and drag
ClipItTo onto Applications.

**First launch:** the app is signed ad hoc, not with an Apple Developer ID, so macOS says it
can't verify the developer. Right-click ClipItTo in Applications, choose *Open*, then *Open*
again (or *Open Anyway* in System Settings › Privacy & Security). From a terminal:
`xattr -dr com.apple.quarantine /Applications/ClipItTo.app`.

ClipItTo lives in the **menu bar** (no Dock icon): click its icon to open the popup, reload
settings or quit.
- **Shortcut:** ⌘⌥V opens the popup (change it in Settings; the config key is `hotkey`, e.g.
  `"Alt+Cmd+V"`: Ctrl is ⌃, Alt is ⌥, Cmd is ⌘). In the popup ⌘S, ⌘P, ⌘G, ⌘E star, pin,
  group and note a copy, and ⌘, opens Settings.
- **Auto-paste** (Enter pastes into the app you came from) presses ⌘V for you, which macOS
  allows only with the **Accessibility** permission: System Settings › Privacy & Security ›
  Accessibility. Without it, Enter still puts the copy on the clipboard. `clipitto --status`
  shows the permission (`accessibility:`). Because the app is signed ad hoc, macOS asks again
  after each update.
- **Keychain:** the history-encryption key and the password-copy key are kept in your login
  keychain (items named `io.github.nimiz.ClipItTo`). After an update macOS may ask whether
  ClipItTo may use them: choose *Always Allow*.
- **Password managers:** copies they mark with nspasteboard.org's `ConcealedType` (1Password,
  KeePassXC, Bitwarden, ...) are kept as [password copies](#password-copies) or skipped;
  `TransientType` and `AutoGeneratedType` copies are never recorded.
- **Start at login:** Settings › *Start ClipItTo when I sign in*.
- **Command line:** `/Applications/ClipItTo.app/Contents/MacOS/clipitto --toggle`, `--status`,
  `--lock`, ... (add a symlink in your PATH if you like:
  `ln -s /Applications/ClipItTo.app/Contents/MacOS/clipitto /usr/local/bin/clipitto`).
- **Files:** settings and history are in `~/Library/Application Support/clipitto`.
- **Uninstall:** untick *Start ClipItTo when I sign in*, quit from the menu bar, delete the
  app, and delete that folder (and the Keychain items, if you used encryption or password
  copies).

Build from source on a Mac (Xcode's command line tools, CMake, Ninja, Qt 6.5+ for macOS):

```bash
export QT_ROOT_DIR=~/Qt/6.5.3/macos
cmake --preset macos && cmake --build --preset macos && ctest --preset macos
cmake --preset macos-release && cmake --build --preset macos-release
PATH="$QT_ROOT_DIR/bin:$PATH" packaging/macos/package.sh build/macos-release build/macos-package
```

## Code signing policy

The Windows release files (`ClipItTo-<version>-setup.exe`, `clipitto.exe`,
`clipitto-daemon.exe`) are being set up for free code signing provided by
[SignPath.io](https://about.signpath.io), with a certificate by the
[SignPath Foundation](https://signpath.org). Until that is in place they are unsigned, and
Windows warns about an unknown publisher; each release lists SHA-256 checksums in
`SHA256SUMS`.

- Releases are built only by this repository's GitHub Actions workflow
  ([ci.yml](.github/workflows/ci.yml)) from the tagged source; nothing is built or signed on a
  personal machine.
- Committers and reviewers: [NiMiZ](https://github.com/NiMiZ)
- Approvers (who start a signed release): [NiMiZ](https://github.com/NiMiZ)

Privacy policy: this program will not transfer any information to other networked systems
unless specifically requested by the user or the person installing or operating it. See
[Privacy](#privacy).

## License

Copyright (C) 2026 NiMiZ

ClipItTo is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License, version 3, as published by the Free Software Foundation. It is
distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE)
for the full text, and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the components it
uses and their licenses.
