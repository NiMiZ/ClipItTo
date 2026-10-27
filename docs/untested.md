# Not yet tested on real hardware

CI builds and unit-tests everything on Linux, Windows and macOS, and runs the packaged apps against
the real clipboard where it can. These need a person at the machine. Tick them off here as they are done.

| Area | What to check | Where |
|---|---|---|
| Touch ID (macOS) | turn it on, lock, unlock with a fingerprint, cancel the prompt, *Forgot it…* reset | a Mac with Touch ID |
| macOS `.dmg` | first launch (Open Anyway), menu-bar icon, ⌘⌥V shortcut, auto-paste with the Accessibility permission, Keychain prompts after an update, start at login | a Mac |
| Windows Hello | turn it on, unlock with face/fingerprint/PIN, cancel, *Forgot it…* reset | Windows with Hello set up |
| Source app of a password copy | the note is the real app's name for KeePass, KeePassXC, Bitwarden, 1Password | Windows, X11, GNOME |
| Pictures | copy and paste back on GNOME Wayland (extension), KDE, wlroots; paste into Office and browsers on Windows | each desktop |
| History encryption | the key kept in a real GNOME Keyring / KWallet / Credential Manager | Linux, Windows |
| Backup across systems | export on one OS, import on another (JSON is plain, so it should work) | any two OSes |
| Wayland drag fix | dragging the popup by its name (issue #3) | Wayland |

Known to work on real machines: paste and Hebrew input on Linux (issue #3), the Windows installer on a
5-monitor desktop, the `.deb` on Ubuntu.
