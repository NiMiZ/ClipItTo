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

- **Meanings of a word (dictionary lookup) — to investigate first.** A translation model gives one reading: for
  "שלום" its six best candidates were all variations of "Peace of peace", never "hello" or "goodbye", because the models
  are trained on sentences. A bilingual dictionary lists the separate meanings; the Translate menu for a single word
  could offer **Meanings…** and paste the one the user picks. Before building it, find out and write down:
  - **Data and licences.** Candidates: Wiktionary extracts (kaikki.org; CC BY-SA 4.0), CC-CEDICT for Chinese
    (CC BY-SA), FreeDict (mostly GPL / CC BY-SA). Which pairs exist and how good they are (Hebrew ⇄ English, Chinese ⇄
    English), whether the licence allows a pack the user adds (not bundled), and what attribution is required. The
    share-alike terms have to be checked against the relicensing checklist in THIRD-PARTY-NOTICES.md.
  - **Size.** The raw extracts are very large; what a compact per-language index weighs on disk (target: well
    under the translation packs' 70-150 MB) and how big a download the user would have to make.
  - **Speed and memory.** An indexed lookup (SQLite, which ClipItTo already has) should take about a millisecond; measure
    it on a real index, together with the memory used and the first-use cost, on Linux, Windows and macOS.
  - **Impact on the app.** A second pack type next to the translation packs (install, remove, list in Settings), no
    cost at startup if loaded lazily, nothing on the clipboard path, and the effect on build and CI time (none if the
    data is a downloaded pack).
  - **UX.** One word or a short phrase only; a menu of meanings with the part of speech; Enter pastes the chosen one;
    what to do for inflected forms (Hebrew prefixes and plurals, English "went" → "go").
  - **Cheaper alternative.** A picker over the translation model's own alternatives needs no new data, but for single
    words it mostly shows the same reading reworded, so it is not worth building on its own.

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
- **DONE (0.11.1): Keyring prompts without an explanation.** A user saw "unlock keyring" dialogs several times and didn't
  know what asked for them (a Linux keyring unlock wants the *keyring's* password, which is often not the
  sudo password). Wanted: say before touching the keyring why ("ClipItTo needs the key that protects your
  history / password copies"), touch it only when a feature needs it, and show in `--status` when a keyring
  call is waiting. Test and driver runs already use a private D-Bus without a keyring.
- **Popup position on Wayland, outside GNOME.** Wayland has no standard way for an app to place its window.
  **DONE on GNOME:** the extension places the popup and centers Settings over it (issue #40; still to be tried
  on a real GNOME session). KDE could do it through KWin scripting, wlroots compositors through layer-shell (corners
  and centre, not "near the pointer"). Elsewhere the setting does nothing and Settings says so.
- **DONE: normalize CRLF when deduplicating.** Copies are stored with LF line ends on every system.
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
8. **DONE: Smart paste keys per app:** ClipItTo picks Ctrl+V / Shift+Insert / Ctrl+Shift+V by the program that gets the
   paste (`paste_keys_per_app`, on by default, a built-in list of terminals; your own `paste_keys_apps` rules, up to 16,
   Settings › General). Supported: Windows (executable name + window class), X11 (`WM_CLASS`), GNOME Wayland with the
   extension (`FocusedApp`: WM_CLASS / app id). Not possible: macOS (always Cmd+V) and KDE / wlroots Wayland (no way to
   ask for the focused window; `paste_keys` is used). The core is `PasteKeys.{h,cpp}`.
9. **Clean links:** strip tracking parameters (`utm_*`, `fbclid`) from copied URLs, optionally.
10. **Text from pictures (OCR):** **DONE: copy the text out of a picture** (right-click › Paste text from picture;
    offline: Windows.Media.Ocr, Vision, Tesseract). Languages: a setting (`ocr_languages`). **DONE: search inside screenshots** (the text is read in the background when a picture is copied, `index_pictures`, on by default). Still open: picking the Tesseract language
    from the picture itself (script detection with `osd` data).
11. **Command line for scripts:** `clipitto --list`, `--get N`, `--add`, `--paste N`, JSON output, so
    other tools and shell scripts can use the history.
12. **Undo delete / Clear** for a few seconds (a toast with "Undo") instead of only asking first.
13. **DONE: Sync without a server** (Settings › Sync, off by default; [design](sync-design.md)): each PC writes one
    encrypted file of its own into a folder the user already syncs (Syncthing, Nextcloud, OneDrive, a share) and merges the
    others'. Not the master password: a separate sync passphrase (Argon2id; only the key is kept, in the keyring). The live
    database is never put there. Text, pictures (size cap), pins, stars, groups, notes and deletions; never password copies,
    secret-scanner copies or Paste once copies. `clipitto --sync` runs one sync. Left open: real sync programs on Windows
    and macOS are untested ([untested.md](untested.md)); group order and Pinned order are not synced; no rich (HTML)
    versions; an automatic merge of two groups made at the same time (needs the passphrase typed again on both PCs).
14. **DONE: Copy with formatting** (Shift+Enter › *With formatting*; `keep_formatting`). Left open: RTF (Word's own flavour, not
    HTML) and backups carrying the formatted version. Original plan: today only plain text was kept, so a copy from Word, a web page or a mail loses
    its bold, links, colours and tables. Wanted: also keep the rich version (HTML / RTF) of a text copy, and
    offer it in the **Paste as** menu as "with formatting" (plain text stays the default). It needs a
    database change (the rich flavour stored next to the text, with a size cap), capture in each backend
    (`text/html` and RTF on Wayland and X11, `HTML Format` and `Rich Text Format` on Windows, `public.html`
    on macOS), and the writer putting both flavours on the clipboard. Encrypted history covers it too.
15. **Previews for more kinds:** **DONE: colour swatch** (`#rrggbb`, `rgb()`...), **file-path list** (names and an icon;
    files copied in Explorer or Finder are recorded as paths) **and QR code view** (right-click › *Show as QR code…*, to
    scan with a phone; MIT library by Project Nayuki). Still open: code highlighting, thumbnails of copied picture
    files, and files copied on GNOME (the extension reads text only).
16. **DONE: Auto grouping** (Settings › *Groups*; `auto_group`, **off by default**). ClipItTo learns from what you move into a
    group and, when it is turned on, files new copies in the group they match best. The groups are only ever the
    ones you made: it never creates a group, and a copy with no clear match stays where it is (in Recent, no group).
    - **Learning.** Every time you put a copy in a group (Ctrl+G, the Group menu), that copy becomes an example for the
      group; copies already in groups count as examples from the start (read once, when the setting is first used).
      Matching is local and simple (the words of the text, and shapes: a URL's host, domain and first folder, a path's
      folders, a command's first word), so it needs no model download and no network. Each token is kept as a 64-bit
      hash, never as text. Nothing is learned while the setting is off.
    - **Choosing.** Each token votes for the groups that had it, in proportion to how often; the group with the most
      matches wins, and only above a threshold and clearly ahead of the runner-up, so a weak guess or a tie is not made.
      Settings shows how many examples each group has (the *Examples* column).
    - **Staying in control.** An automatically filed copy is marked (a small "auto" tag on its group chip) so it can be
      told from one you filed; moving it to another group, or removing it from the group, teaches the model the other
      way (a correction counts three examples; a filed-by-guess copy is never an example itself). Turning the setting off
      keeps the filed copies, and a *Forget what was learned* button clears the examples (the copies in groups are not
      read as examples again afterwards).
    - **Limits.** Never for password copies (marked, detected, by a rule) or pictures, and they teach nothing when
      filed by hand. Nothing learned leaves the history file (it is encrypted with it; a new migration, 009). It runs when
      a copy is stored, on a worker thread against an in-memory snapshot, and the work per copy is bounded (first 4000
      characters, at most 48 tokens). Only a copy that is still unsaved and unfiled when the worker is done is filed.
    - **Decided:** only new copies are filed (no one-time "File what matches" button), and a copy matches one group, the
      best one (no menu to pick another; move it yourself and it learns). Not done: the app a copy came from is not
      used, because ClipItTo does not keep it with a copy (only for password notes), so it would match nothing; and
      "Mark as password's group" teaches nothing, since password copies never do.

## More ideas (researched November 2026)

From a look at CopyQ, Ditto, Maccy, Klipper and Windows' own clipboard history, kept to what fits ClipItTo: nothing leaves the
computer unless the user asks, no clipboard contents in logs, no new library without approval. Nothing here is promised or scheduled.
Items are numbered to carry on from the list above. Ones already listed there (quick paste keys, snippets, paste queue, ignore apps,
command line, undo, clean links, sync through a folder, smart paste keys per app, `app:` filter, code highlighting) are not repeated.

### Privacy and safety
17. **DONE: Paste once** (`Ctrl+Shift+Enter`, *Paste once* in the menus, `paste_once_clear_seconds`). *Details below.* Built on the
    clearing mechanism of the password work: `ClipboardWriter::clearPasted()` (the old `clearPassword()`) now also handles Paste once
    text and pictures. The automatic rules (by pattern or app) are not done.
18. **DONE: Auto-forget:** copies that expire (`auto_forget`, Settings › Passwords › *Auto-forget*). *Details below.*
19. **PARTLY DONE: Password-field copies on Windows and macOS.**
    *Research in [password-field-research.md](password-field-research.md). Done: the marker list is extended (KeeWeb as a secret; three more transient pasteboard types), with tests. Not done, on purpose: Windows UI Automation `IsPassword` and macOS `IsSecureEventInputEnabled` as signals (neither is reliable at copy time; no setting added).* Some browsers set no password-manager marker when a password is copied from a
    field. Today only the generated-password rule (off by default) and the user's own rules catch those. Look for other signals
    (the source window's class or title when it is a browser password field, a copy made right after an autofill) and see what
    each platform really offers; do nothing that reads more of the clipboard than now.
20. **DONE: Panic key** (`panic_key`, `panic_hotkey`, `clipitto --panic`; Settings › Passwords). *Decided with the user:* ONE action, no
    confirmation, that (1) wipes the history, (2) locks the passwords and (3) also empties the system clipboard; OFF by default.
    - *Wipe* is the existing *Clear everything, also in groups* (`Repository::clearHistory(true)`, not a second code path): all copies
      including pinned, starred, grouped and password copies, pictures, rich text, learned auto-grouping data, search index merged,
      pages zeroed (`secure_delete`), WAL folded in; plus the popup's Paste once undo buffer, thumbnails and revealed passwords, and a
      pending clipboard clear. *Kept:* the groups (empty), the settings, the vault and the master password (locked, not deleted).
    - *Lock* is `PasswordKeeper::lock` (same as `--lock`); the "no master password" mode cannot lock (its copies are deleted anyway).
    - *Clipboard:* `ClipboardWriter::clearAll()` empties it unconditionally (text, picture, files, anything; X11 primary too; macOS
      `clearContents`; GNOME extension asked for an empty clipboard, no read-back). `Unsupported` where the system refuses
      (Wayland without focus/helper), logged as a flag only.
    - *Triggers:* the shortcut (only while `panic_key` is on and `panic_hotkey` is set and differs from `hotkey`; Windows/macOS in the
      daemon with their own hotkey id, GNOME a second custom shortcut `clipitto-panic`, elsewhere bind the command; conflicts show in
      `--status` as `panic_hotkey: unavailable: ...`), and `clipitto --panic` / D-Bus `Panic` / local socket `Panic`, which work
      whatever the setting says and print `done`.
    - *After:* popup hides and shows an empty list; translations, text-from-picture reads, picture decodes and queued captures that
      were started before are ignored (a half-second quiet period for captures on their way); nothing logged but flags and a count;
      a panic during an encryption conversion empties the clipboard and locks at once and wipes when the conversion ends.
    - *Not done:* clearing other programs' clipboard histories (Windows' own Win+V history is unaffected by the clipboard clear
      unless the OS honours it), and screen-lock triggers.

### Finding things
21. **Source app and time on every row** (needed by the `app:` filter above and by auto grouping by app). ClipItTo stores the
    source app only for password copies today. Open: one more column and its cost for 50,000 copies, privacy of the list of apps
    (encrypted with the history), and platforms where the source cannot be found (Wayland outside GNOME).
22. **Duplicate finder and "similar to this".** Copies that differ only by spaces, case or a trailing newline are shown together
    with a way to keep one.
23. **Saved searches.** Pin a query such as `group:work since:today` as a button in the popup.
24. **Regex search** behind a prefix such as `re:` (Ditto offers it). Open: a time limit so a bad pattern cannot freeze the popup.

### Pasting
25. **More Paste as transforms:** camelCase, snake_case, kebab-case, slugify, remove duplicate lines, sort lines, number lines,
    quote lines, wrap as a markdown link or code block. They extend `TextTransform` and the `paste_as_*` lists.
26. **Unwrap hard line breaks** from PDFs and e-mails (join the lines of one paragraph, keep blank lines).

### Between computers
27. **Send a copy to another of your computers** on the local network, after pairing with a short code, encrypted, off by default
    (Ditto has "Friends"). It would be the first network use by ClipItTo, so it needs its own decision and a clear setting.
28. **Receive text from a phone** with a short-lived local page opened from a QR code. A bigger step than the QR code that
    shows a copy; same network question as the item above.

### Keeping the clipboard useful
29. **Snapshots:** a named set of copies ("project X") that can be put back later, or pasted as a queue.
30. **Thumbnails of copied picture files** and files copied on GNOME (the extension reads text only). Open items from previews.

### Quality and trust
31. **A self-test** (*Check this installation*): is the hotkey registered, is the keyring reachable, does capture see a test
    copy, is the diagnostic log writable, does paste work into a test box. It would have shortened the name-box investigation.
32. **A small log viewer** for the diagnostic log (Settings), with a button that copies it to the clipboard for sharing.
33. **Longest GUI pause in `--status`** since the program started, so a stall report has a number.

### Paste once (17): details
**Done (by hand; the automatic rules are still open).** Decisions made with the user:
- The copy is *stored* like any other (migration 010, `clips.paste_once`; encrypted with the history), survives a restart, and wears a
  small **once** tag. Marked with *Paste once* (a toggle) in the right-click and Shift+Enter menus; `Ctrl+Shift+Enter` pastes the
  selected copy and forgets it in one go, marked or not (no existing shortcut used it; `Ctrl+Shift+Z` is the Undo).
- Removed right *after* the paste keys were sent (`Daemon::pasteIfWanted` reports whether they went out), with the secure delete of
  *Clear history* (`Repository::forgetNow`: `secure_delete`, index merge, WAL checkpoint). If the keys were not sent (paste on Enter off,
  blocked, unsupported) nothing is deleted and the clipboard is left alone.
- The clipboard is emptied `paste_once_clear_seconds` (default 2, 0 = never, 0-60, Settings › Passwords next to the password one) after
  the paste, through the same code as pasted passwords: only if it still holds that paste (text is marked like a password and compared
  by keyed hash; a picture by a hash of its pixels), never a newer copy, not recorded as a copy, done at quit if pending.
- A password copy: pasted first, then *Delete this password copy?* every time (`PopupWindow::setDeletePasswordPrompt`, injectable);
  *No* keeps it and removes the mark. Pictures: paste, then delete. Pinned and starred copies are never deleted: the mark is removed and
  the popup says so. Grouped copies are deleted.
- Undo for 10 seconds in the popup (button and `Ctrl+Shift+Z`): `Repository::snapshot` / `restore` put back text or picture, group,
  note, times (a password copy as its sealed blob); the mark is not restored. The snapshot is kept in memory only for those 10 seconds.
- Not in backups: export skips once copies, import ignores any item flagged `once`.
- *Paste as* transforms and *With formatting* do not use up a marked copy.
Tests: `tst_pasteonce` (repository, backup, popup, daemon), `tst_passwordclear` (writer and filter), `tst_migrator`, `tst_config`,
`tst_settings`, `tst_gui_general`. Untested on real systems: see `docs/untested.md`.


**What it is.** A copy marked *Paste once* is removed from the history right after it has been pasted from ClipItTo, and the
system clipboard is cleared if it still holds it. For one-time codes, a password typed into a stranger's form, or a private
number that should not stay anywhere.

**How a copy becomes one-time**
- By hand: *Paste once* in the right-click and Shift+Enter menus (and a key, e.g. Ctrl+Shift+Enter, to paste and forget in one go).
- Automatically (a setting, off by default): copies that match a pattern, such as a 6-digit code, or copies from chosen apps
  (needs item 21). Same rule editor idea as `password_rules`.

**What "pasted" means.** ClipItTo can only know about its own paste action, when it puts the text on the clipboard and sends the
paste keys. A Ctrl+V the user presses later in another program is not seen. So the system clipboard is cleared a few seconds after
ClipItTo's paste (`paste_once_clear_seconds`, default 5), not at the next Ctrl+V.

**Decisions to make first**
1. **Decided: stored.** A *Paste once* copy is kept like any other copy (encrypted with the history if that is on) until it is used.
   It survives a restart and is removed after its paste.
2. **Decided: a password copy is never deleted without asking.** *Paste once* on a password copy asks before the sealed copy is
   deleted, every time (it pastes first; then asks "Delete this password copy?"), and *No* keeps it.
3. *If the paste did not land* (wrong window, paste keys blocked): delete only after the paste keys were sent; keep a short
   *Undo* in the popup for 10 seconds.
4. *Clipboard clearing per system:* Windows (empty the clipboard with the same exclude-from-history markers the paste uses),
   macOS (clear the pasteboard when it still holds our change count), X11 (take and empty the selection), Wayland/GNOME
   (the extension clears it), wl-paste (`wl-copy --clear`). Never clear a clipboard that already holds something newer.
5. *Backups and history search:* never in a backup; never found by search after use.
6. *What the row shows:* a small *once* tag; the text is shown as for password copies when the setting says so.

**Tests to write:** deleted after the paste, kept if the paste was cancelled, clipboard cleared only when unchanged, not in a
backup, survives (or not) a restart as decided, and the auto rules.

### Auto-forget (18): details (nice to have)
Copies that remove themselves. Rules, like `password_rules`, in Settings › Privacy and in the config file (`auto_forget`):
- by **time**: all copies older than N days (outside pinned and saved), or copies of a kind (pictures after 1 day);
- by **pattern**: copies matching a pattern are removed after N minutes (e.g. text containing `Bearer ` after 5 minutes), reusing
  the secret scanner's kinds (token, key, card number) as ready-made choices;
- by **app** (needs item 21): copies from a chosen program are removed after N minutes.
Pinned, starred and grouped copies are never removed. A removal leaves nothing in the log but a count. Open: whether a removal
is shown (a toast) or silent, and whether the prune runs on a timer or when the popup opens.

**Done (the time and pattern rules; by app is still open).** Up to 8 rules, none by default. Written `age 30d`, `age 1d picture`,
`age 7d text`, `match 5m contains "Bearer "`, `match 10m token|key|card|secret`. Decisions: removals are silent (no toast); the log
gets only a count at debug level; the prune runs at start, once a minute, when the popup opens and when the rules change, on the
GUI thread in small steps (at most 100 copies deleted and 500 read per step, the rest on the next step) because the history has
one connection. Deleted rows are removed like *Clear history* (secure delete, search index merged, log folded in). Pinned, starred,
grouped and password copies are never matched, and a password copy's text is never read. A time rule counts from the copy's last
use. **By app** waits for item 21 (the source app is stored for password copies only). Code: `src/core/AutoForget.*`; tests
`tst_autoforget`, `tst_daemon`, `tst_settings`, `tst_config`, `tst_perf_data`.

**Clipboard clearing (done for pasted passwords).** `password_clear_seconds` (default 2, 0 = off, 0-60) empties the clipboard that
long after a password paste, only if the clipboard still holds it: Windows and X11/Wayland (Qt) read it back and clear only if
it still holds the password (compared by hash), macOS compares the pasteboard's change count, and through the GNOME extension (which
cannot be read back) a newer copy seen by the capture is the only check. A pending clear also runs when the program quits. The
clear is never recorded as a copy. Not done: `wl-copy --clear` for the wl-paste backend (the repo has no `wl-copy` channel; writes
there go through Qt), and the extension clearing on its own.
