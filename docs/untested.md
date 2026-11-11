# Not yet tested on real hardware

CI builds and unit-tests everything on Linux, Windows and macOS, and runs the packaged apps against
the real clipboard where it can. These need a person at the machine. Tick them off here as they are done.

| Area | What to check | Where |
|---|---|---|
| Touch ID (macOS) | turn it on, lock, unlock with a fingerprint, cancel the prompt, *Forgot it…* reset | a Mac with Touch ID |
| macOS `.dmg` | first launch (Open Anyway), menu-bar icon, ⌘⌥V shortcut, auto-paste with the Accessibility permission, Keychain prompts after an update, start at login | a Mac |
| Windows Hello | turn it on, unlock with face/fingerprint/PIN, cancel, *Forgot it…* reset | Windows with Hello set up |
| Source app of a password copy | the note is the real app's name for KeePass, KeePassXC, Bitwarden, 1Password | Windows, X11, GNOME |
| Copy with formatting | copy from Word, a web page and a mail, then *Shift+Enter › With formatting* into Word / a mail editor; the plain paste stays plain | Windows (HTML Format), macOS (public.html), GNOME extension, KDE / wlroots (wl-paste), X11 |
| Copied files | copy files in Windows Explorer and in Finder: the row shows the file names, pasting gives the paths | Windows, macOS |
| Pictures | copy and paste back on GNOME Wayland (extension), KDE, wlroots; paste into Office and browsers on Windows | each desktop |
| History encryption | the key kept in a real GNOME Keyring / KWallet / Credential Manager | Linux, Windows |
| Backup across systems | export on one OS, import on another (JSON is plain, so it should work) | any two OSes |
| Auto grouping | the **auto** tag on a group chip looks right in light and dark themes and at large font sizes; in a real session a copy from a browser lands in the group learned from earlier links | any desktop |
| Copies from real apps (formats) | CI simulates the data each app puts on the clipboard. Check with the real apps: Word and Excel on Windows (the real *HTML Format* offsets, CF_HDROP, a password manager's `ExcludeClipboardContentFromMonitorProcessing`), Chrome and Firefox (HTML with `<meta charset>`), Explorer files, a screenshot (Win+Shift+S), a browser *Copy image* | Windows |
| Copies from real apps (macOS) | Finder file copy (file URLs), 1Password and KeePassXC (`org.nspasteboard.ConcealedType`), Pages / browsers with formatting (`public.html`), Terminal | a Mac |
| Copies from real apps (Linux) | Nautilus `x-special/gnome-copied-files`, KeePassXC and Bitwarden (`x-kde-passwordManagerHint`), LibreOffice and Firefox HTML through `wl-paste` on KDE / wlroots, VS Code (`vscode-editor-data`) | X11, Wayland |
| CRLF vs LF | the same text copied with Windows and Unix line breaks on Linux is two entries today (optional item in the roadmap); on Windows and macOS the readers make it one | Windows, Linux |
| Wayland drag fix | dragging the popup by its name (issue #3) | Wayland |

Known to work on real machines: paste and Hebrew input on Linux (issue #3), the Windows installer on a
5-monitor desktop, the `.deb` on Ubuntu.
