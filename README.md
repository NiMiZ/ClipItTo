# ClipItTo
Clipboard manager with history, groups and favorites for Linux, Windows and macOS

Press a shortcut, type a few letters, press Enter: the clip (text or picture) is back on
your clipboard.

| Your system | Get it | Details |
|---|---|---|
| **Ubuntu 24.04+** | `curl -fsSL https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.sh \| sh` | [Linux](docs/install.md#linux) |
| **Other Linux** | AppImage or Snap from the [latest release](https://github.com/NiMiZ/ClipItTo/releases/latest) | [Linux](docs/install.md#linux) |
| **Windows 10/11** | `irm https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.ps1 \| iex` in PowerShell | [Windows](docs/install.md#windows-1011) |
| **macOS 11+** | `ClipItTo-<version>-macos.dmg` from the [latest release](https://github.com/NiMiZ/ClipItTo/releases/latest) | [macOS](docs/install.md#macos-11) |

The default shortcut is **Ctrl+Alt+V** (**⌘⌥V** on a Mac).

## What it does

- **History** of text and pictures, searchable by text, note and group.
- **Pin** up to 10 clips to the top, **star** clips into Favorites, sort clips into your own
  **groups** and sub-groups. Pinned, starred and grouped clips are never pruned.
- **Notes** give a clip a readable title.
- **Password copies** from password managers are kept encrypted behind a master password, never in clear.
- **Encrypted history** on disk (optional), and **backup** of copies and settings to one file.
- **Private:** no network, no cloud, no telemetry.

## Keys

| Key | Action |
|---|---|
| type | filter history |
| `↑` / `↓`, `Enter` | move, copy the entry back and paste it into the window you came from |
| `Shift+Enter` | paste as…: upper / lower case, JSON pretty, URL or Base64 encode, wrong keyboard layout (`xccv` → `סבבה`), and more |
| `Ctrl+S` / `Ctrl+P` | star / pin |
| `Ctrl+G` / `Ctrl+E` | set group / edit note and text |
| `F2` | rename the group, or edit the note of the copy |
| `Del` | delete (or take it off Recent) |
| right-click | the same actions in a menu |
| `img:` `pin:` `star:` `group:x` `since:3d` | search filters, next to your words |
| `Ctrl+,`, `Esc` | settings, close |

## More

| | |
|---|---|
| [Installing](docs/install.md) | `.deb`, AppImage, Snap, Windows installer, macOS app: all the steps |
| [Using it](docs/usage.md) | groups, stars, pins, Recent, duplicates, backup, pictures, settings |
| [Encryption and password copies](docs/passwords.md) | how they are protected, what isn't covered |
| [Translation](docs/translation.md) | offline translation of text and of text in pictures (Chinese ⇄ English, Hebrew ⇄ English, ...) |
| [Capture per session](docs/platforms.md) | Wayland, X11, Windows, and the GNOME extension |
| [Building from source](docs/building.md) | Linux, Windows, macOS |
| [config.example.toml](data/config.example.toml) | every setting, with comments |

## Privacy

Your clipboard history stays on your computer. ClipItTo makes no network connections: no
cloud sync, no accounts, no telemetry, no update checks. Copies, notes, groups and settings
are stored only in local files under your user account (`~/.local/share/clipitto/` and
`~/.config/clipitto/` on Linux, `%LOCALAPPDATA%\clipitto\` and `%APPDATA%\clipitto\` on
Windows), readable by your account only; turn on [encryption](docs/passwords.md#encrypted-history) to
protect them at rest as well. The daemon, the CLI and the GNOME extension talk to each
other over your session's D-Bus or a local socket, never over the network. Logs never
contain clipboard contents. Copies that password managers mark as secret are skipped, or kept
only encrypted once you set a [master password](docs/passwords.md#password-copies). To erase everything, quit ClipItTo and delete those folders.

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
