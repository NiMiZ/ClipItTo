# Roadmap

Known gaps first, then ideas. Nothing here is promised or scheduled. See also
[untested.md](untested.md) for what still needs a person with the hardware.

## Next

- **DONE (0.8.0, Hebrew / Russian / Greek; Arabic still open): Paste as… another keyboard layout.** Text typed with the wrong layout, same keys, other language: copy
  `xccv`, choose *Paste as › Hebrew keyboard*, and `סבבה` is pasted (and the other way: `akuo` as English
  keyboard becomes `שלום`). It maps each key to the character the same physical key gives in the other
  layout, so it needs no dictionary and no network. Plan: a table per layout pair, starting with the standard
  English (US) and Hebrew layouts (`q→/`, `w→'`, `e→ק`, `r→ר`, `t→א`, `y→ט`, `u→ו`, `i→ן`, `o→ם`, `p→פ`,
  `a→ש`, `s→ד`, `d→ג`, `f→כ`, `g→ע`, `h→י`, `j→ח`, `k→ל`, `l→ך`, `;→ף`, `z→ז`, `x→ס`, `c→ב`, `v→ה`, `b→נ`,
  `n→מ`, `m→צ`, `,→ת`, `.→ץ`, `/→.`), both directions, shifted keys and punctuation included. The menu offers
  the direction that fits the text (Latin letters → "as Hebrew keyboard", Hebrew letters → "as English
  keyboard"). Later: Arabic, Russian, Greek and others, and the layouts actually installed on the computer.
  It extends the existing Paste as… menu (`TextTransform`), with unit tests for every key of each table.

## Translation (0.9.0)

- **DONE: offline translation** of text and of the text in pictures (Settings › Translation; Shift+Enter ›
  Translate). Language packs are added from a file the user downloads.
- **Open:** an in-app "download this pack" button (would make ClipItTo's only network connection; off unless the
  user presses it), a short list of recommended packs, translating a picture in place (an overlay), and more
  pairs through English (Chinese → Hebrew goes via English today only if both packs are installed).

## Known gaps

- **Passwords don't lock with the screen.** Today: `clipitto --lock` from a screen-lock hook, or a short
  unlock time. Wanted: lock on screen lock and on sleep (logind / `NSWorkspace` / `WTS` session events).
- **Linux has no fingerprint unlock and no "Forgot it…" reset.** fprintd and polkit only answer yes or no
  and hold no key. A copy of the unlock key in the system keyring is possible but only as strong as the
  login session; it would be off by default with a warning.
- **Touch ID is a gate, not a lock.** A Keychain item bound to the fingerprint needs a developer
  certificate (a keychain access group). Wanted: switch to it when the app is signed.
- **Code signing.** Windows installer unsigned until SignPath approves; macOS ad-hoc signed, so permissions
  are asked again after each update (a Developer ID would fix this and allow notarization).
- **Backup:** "same password per note" makes a new copy if a note is renamed and the password re-copied.
- **Pictures:** Windows paste-back decodes the PNG on the GUI thread; the hover preview is the 320 px
  thumbnail (blurry on HiDPI).
- **Security:** text over D-Bus (`Ingest`) has no sender check.
- **KDE, Sway and others:** no shortcut helper; users bind `clipitto --toggle` themselves.
- **Keyring prompts without an explanation.** A user saw "unlock keyring" dialogs several times and didn't
  know what asked for them (a Linux keyring unlock wants the *keyring's* password, which is often not the
  sudo password). Wanted: say before touching the keyring why ("ClipItTo needs the key that protects your
  history / password copies"), touch it only when a feature needs it, and show in `--status` when a keyring
  call is waiting. Test and driver runs already use a private D-Bus without a keyring.
- **Popup position on Wayland, outside GNOME.** Wayland has no standard way for an app to place its window.
  **DONE on GNOME:** the extension places the popup and centers Settings over it (issue #40; still to be tried
  on a real GNOME session). KDE could do it through KWin scripting, wlroots compositors through layer-shell (corners
  and centre, not "near the pointer"). Elsewhere the setting does nothing and Settings says so.
- **Optional:** normalize CRLF when deduplicating.
- **Open question:** "search doesn't find custom groups" was reported once and could not be reproduced.

## Ideas for productivity

Ordered by how much they would likely help, not by effort. Items marked **DONE** are released (0.7.0 and 0.8.0).

1. **DONE: Transform on paste** (ClipItTo stores plain text only, so formatting is never pasted anyway): UPPER / lower / Title case, trim, collapse whitespace, JSON pretty/minify, URL decode/encode.
2. **Quick paste keys** (`Ctrl+1`…`9` in the popup, or a global chord) for the pinned copies, so a
   frequent snippet is one chord away without opening the list.
3. **Snippets with placeholders:** pinned copies that expand `{date}`, `{time}`, `{clipboard}` or ask for a
   value when pasted (email replies, commands, addresses).
4. **Paste queue:** select several copies, then paste them one after another with one chord (filling a
   form, moving rows between apps). Also **join**: merge selected copies into one with a separator.
5. **Ignore apps:** a list of programs whose copies are never recorded (banking, remote desktop), on top of
   the password-manager rule. Related: **auto-forget** copies after N minutes for chosen apps or patterns.
6. **DONE: Sensitive-pattern guard:** copies that look like API keys, tokens, card numbers or private keys are
   offered as password copies (or skipped) automatically instead of sitting in plain history.
7. **DONE (except `app:`): Search filters:** `img:`, `text:`, `pin:`, `star:`, `group:`, `note:`, `today:`, `since:`.
   Still open: `app:chrome` and showing the source app and time on a row; both need the source app stored for
   every copy (it is captured only for password copies today).
8. **Smart paste keys per app:** pick Ctrl+V / Shift+Insert / Ctrl+Shift+V automatically for terminals
   instead of one global setting.
9. **Clean links:** strip tracking parameters (`utm_*`, `fbclid`) from copied URLs, optionally.
10. **Text from pictures (OCR):** **DONE: copy the text out of a picture** (right-click › Paste text from picture;
    offline: Windows.Media.Ocr, Vision, Tesseract). Languages: a setting (`ocr_languages`). Still open: search inside screenshots (read the text when a
    picture is copied and index it; needs a database change and a setting), and picking the Tesseract language
    from the picture itself (script detection with `osd` data).
11. **Command line for scripts:** `clipitto --list`, `--get N`, `--add`, `--paste N`, JSON output, so
    other tools and shell scripts can use the history.
12. **Undo delete / Clear** for a few seconds (a toast with "Undo") instead of only asking first.
13. **Sync without a server:** export/import to a folder the user already syncs (Syncthing, Nextcloud),
    encrypted with the master password, keeping the "no network from ClipItTo" promise.
14. **Copy with formatting:** today only plain text is kept, so a copy from Word, a web page or a mail loses
    its bold, links, colours and tables. Wanted: also keep the rich version (HTML / RTF) of a text copy, and
    offer it in the **Paste as** menu as "with formatting" (plain text stays the default). It needs a
    database change (the rich flavour stored next to the text, with a size cap), capture in each backend
    (`text/html` and RTF on Wayland and X11, `HTML Format` and `Rich Text Format` on Windows, `public.html`
    on macOS), and the writer putting both flavours on the clipboard. Encrypted history covers it too.
15. **Previews for more kinds:** colour swatch for `#rrggbb`, QR code view of a copy, code highlighting,
    file-path list for copied files.
