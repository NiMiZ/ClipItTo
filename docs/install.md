# Installing ClipItTo

Back to the [README](../README.md).

# Linux

**Ubuntu 24.04+ (`.deb`).** One command downloads the latest `.deb`, checks its SHA-256
against the release and installs it with apt, which pulls in the Qt libraries it needs:

```bash
curl -fsSL https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.sh | sh
```

Or get `clipitto_<version>_amd64.deb` from the
[latest release](https://github.com/NiMiZ/ClipItTo/releases/latest) and run
`sudo apt install ./clipitto_0.6.0_amd64.deb`. [install.sh](../install.sh) also takes
`--version X.Y.Z` and `--download-only DIR`.

Then, once per user, **log out and back in** (so GNOME Shell sees the extension) and run
`clipitto-setup`. It starts the daemon with your session, enables the GNOME extension and
binds **Ctrl+Alt+V** (pass another binding, e.g. `clipitto-setup '<Super>v'`).
`clipitto-shortcut [BINDING]` changes the shortcut later; `clipitto-shortcut --remove`
removes it. Remove ClipItTo with `sudo apt remove clipitto`.

**Other distributions.** Two downloads carry everything inside (Qt and the other libraries),
so nothing else needs installing. Both are on the
[latest release](https://github.com/NiMiZ/ClipItTo/releases/latest).

- **AppImage** (`ClipItTo-<version>-x86_64.AppImage`): one file for most distributions from
  2024 on (glibc 2.39+: Ubuntu 24.04, Fedora 40, Debian 13, Arch, ...).

  ```bash
  chmod +x ClipItTo-*-x86_64.AppImage
  ./ClipItTo-*-x86_64.AppImage --install    # start with the session, menu entry, GNOME extension, shortcut
  ```

  `--install` sets it up for your account, pointing at the AppImage where it is (keep it
  there); on GNOME, log out and back in and run `--install` once more to enable the
  extension. The AppImage also takes every `clipitto` option (`--toggle`, `--status`,
  `--lock`, ...). `--uninstall` undoes the setup and keeps your history. Without FUSE (some
  containers), set `APPIMAGE_EXTRACT_AND_RUN=1`.

- **Snap** (`clipitto_<version>_amd64.snap`), strictly confined:

  ```bash
  sudo snap install --dangerous ./clipitto_*_amd64.snap
  ```

  `--dangerous` is needed while it isn't in the Snap Store. Then run `clipitto.setup` once
  (ClipItTo starts with your session from then on), start it now with
  `clipitto.clipitto-daemon &`, and bind `clipitto --toggle` to a shortcut. On GNOME, the
  extension is not part of the snap; install it from the `.deb` or the AppImage (`--install`).

On **GNOME Wayland** (the Ubuntu default) the daemon can only see copies made while its own
window has focus until the GNOME extension is installed (`backend_degraded: true` in
`--status`); the `.deb` and the AppImage install it. On **Xorg** no extension is needed.

# Windows 10/11

**Install:** in PowerShell, run

```powershell
irm https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.ps1 | iex
```

It downloads the installer, checks its SHA-256, installs ClipItTo for your account with
"Start ClipItTo when I sign in", and starts it. ([install.ps1](../install.ps1) also takes
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
[config.example.toml](../data/config.example.toml); `hotkey = "Ctrl+Shift+V"` changes the
shortcut), history in `%LOCALAPPDATA%\clipitto\history.db`. `clipitto.exe --status`,
`--reload` and `--quit` (in the install folder's `bin`) work as on Linux.

# macOS 11+

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
- **Touch ID:** Settings › Passwords › *Unlock with Touch ID too* (see [how it protects](passwords.md)).
- **Password managers:** copies they mark with nspasteboard.org's `ConcealedType` (1Password,
  KeePassXC, Bitwarden, ...) are kept as [password copies](passwords.md#password-copies) or skipped;
  `TransientType` and `AutoGeneratedType` copies are never recorded.
- **Start at login:** Settings › *Start ClipItTo when I sign in*.
- **Command line:** `/Applications/ClipItTo.app/Contents/MacOS/clipitto --toggle`, `--status`,
  `--lock`, ... (add a symlink in your PATH if you like:
  `ln -s /Applications/ClipItTo.app/Contents/MacOS/clipitto /usr/local/bin/clipitto`).
- **Files:** settings and history are in `~/Library/Application Support/clipitto`.
- **Uninstall:** untick *Start ClipItTo when I sign in*, quit from the menu bar, delete the
  app, and delete that folder (and the Keychain items, if you used encryption or password
  copies).
