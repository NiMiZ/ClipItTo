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
| Clearing a pasted password | paste a password copy, wait `password_clear_seconds`: the clipboard is empty; copy something else within the time: it stays. Windows (clipboard empties, Windows' clipboard history shows nothing), macOS (change count compare, Concealed password then cleared), X11, KDE/wlroots Wayland (Qt needs focus to write; after the popup closes the clear may be refused and the log says so), GNOME extension (no read-back: only the capture's newer-copy check), quit with a paste pending | each system |
| Paste once | on each system, mark a copy (right-click › *Paste once*), paste it into a text box with Enter: the copy disappears from the popup, the clipboard is empty after `paste_once_clear_seconds`, *Undo* in the popup brings it back; `Ctrl+Shift+Enter` on an unmarked copy; a password copy asks *Delete this password copy?* after pasting; a picture (not marked as excluded from Windows clipboard history, unlike text; macOS read-back may differ in alpha so the clear is skipped; Windows: the clipboard's bitmap may not read back pixel-identical, then the picture is not cleared: check); paste keys blocked or `paste_on_enter = false`: the copy stays; the **once** tag looks right in light and dark themes; GNOME extension (clear cannot be read back: only the capture's newer-copy check) and KDE/wlroots Wayland (Qt may refuse the clear without focus; the log says so) | each system |
| Auto-forget | on a real desktop: add `match 1m token` and copy a made-up token without a master password; it disappears from the popup within about a minute and the history file keeps no trace (`strings history.db*`) | Linux, Windows, macOS |
| Paste keys per program | with `paste_keys_per_app` on, open the popup over a terminal and paste a copy: Windows (Windows Terminal, classic cmd / PowerShell console, mintty, Git Bash, VS Code's terminal with a rule), X11 (GNOME Terminal, Konsole, xterm, kitty, Alacritty), GNOME Wayland with the extension (log out and in after updating it; GNOME Terminal / Console / Ptyxis, a Wayland-native app id such as `org.gnome.Terminal`); a normal program (browser, editor) still gets Ctrl+V; an `xprop WM_CLASS` name in a `paste_keys_apps` rule; KDE / wlroots Wayland and macOS: the global keys (nothing to choose); the Settings editor looks right in light and dark themes | Windows, X11, GNOME Wayland |
| Wayland drag fix | dragging the popup by its name (issue #3) | Wayland |

Known to work on real machines: paste and Hebrew input on Linux (issue #3), the Windows installer on a
5-monitor desktop, the `.deb` on Ubuntu.
