# Using ClipItTo

Back to the [README](../README.md).

| Key | Action |
|---|---|
| type | filter history (note, group and text) |
| `↓` / `↑` | move the selection (it starts on the latest copy: `↓` next copy, `↑` the Groups row); `↑` on the top row returns to the search bar |
| `Shift+Enter` | **Paste as…** a menu of changes to the text for this paste only: UPPER, lower, Title Case, trim, collapse spaces, one line, JSON pretty / minified, URL and Base64 encode / decode, and **keyboard layout** (below). Press the underlined letter. The stored copy is not changed |
| `Enter` / click | copy the entry back to the clipboard and close (on a group: open/close it) |
| `Ctrl+Shift+Enter` | **Paste once:** paste the selected copy and forget it, marked or not (see *Paste once* below) |
| `→` / `←` | open / close a group; `→` in the empty search box opens **Groups** |
| `Ctrl+S` / `Ctrl+P` | star / pin to the top (again to undo) |
| `Ctrl+G` / `Ctrl+E` | assign group (on a group: rename) / edit the note and the text of a copy (see below) |
| `F2` | rename: on a group its name (same as `Ctrl+G` there), on a copy its note (same as `Ctrl+E`); also from the search box |
| `Alt+↑` / `Alt+↓` | move a group, or a pinned copy, up or down |
| `Del` | delete the entry (on a group: delete the group, its copies are kept); in the search box too, when the cursor is at the end |
| right-click or `Menu` | the same actions in a menu, plus new group / sub-group on groups |
| `Ctrl+,` or ⚙ | settings |
| `Esc` | close |

**Editing a copy (Ctrl+E).** One dialog with two boxes: **Note** (Enter saves) and **Text** (Ctrl+Enter saves,
Enter adds a line). Pictures and password copies have only the Note box. Saving the text changes the copy in
place: its note, group, star, pin, position in Recent and "last used" time stay. The text can't be empty, over
`max_item_bytes`, or the same as another copy's; the dialog says so and nothing changes. If the edited copy is
the most recent one, the new text is also put on the system clipboard (no new entry is made from it) and a
notification says so (the desktop's notification service on Linux, the tray message on Windows and macOS, or a
line in the popup if neither works). Esc cancels.

**Keyboard layout (Paste as…).** Text typed with the wrong layout becomes the other language on the same keys:
copy `xccv`, press Shift+Enter, and *As Hebrew keyboard: סבבה* pastes `סבבה` (and `סבבה` → `xccv`). Choose the
languages you type in at Settings › General › *Keyboard layouts for Paste as…* (Hebrew, Russian, Greek; empty
means the language of your system). With one language the menu offers the opposite of the text's language
(English ⇄ that language); with several it shows a submenu of them, each with a preview of the result. Russian
and Greek keep capital letters. It works key for key, with no dictionary and no network.

**Numbers (Paste as… › Number).** For a copy that is one number the menu has a *Number* submenu, grouped by how the text is read.
A `0x` prefix or hex letters (`2c`) mean hex only; digits only (`123`) can be both, so you get a **Read as hex** group and a
**Read as decimal** group, each with the number in the other base, *As bits* (padded to whole bytes: `2c` becomes `00101100`) and a
grey *Bits set* line (0-based, highest first: `0x123` gives `8 5 1 0`). Click a line to paste it; the grey lines are information.
Numbers above 64 bits are ignored.

**Choosing the Paste as… items.** Settings › *Paste as* lists the menu's items, one list for text and one for
pictures: tick an item to show it, untick to hide it, move it with *Move up* / *Move down* (or drag it), and
*Reset to default* restores the built-in list and order. The menu ends with *Others*, a submenu with the unticked items (turn it off with *Show the other items in the menu*; `paste_as_others` in the config). Shift+Enter opens the menu with the first item selected, so Enter picks it. The right-click *Paste as* submenu follows the same
list. When two shown items would use the same underlined letter, the later one loses it. With every item hidden
the menu shows only "No items selected". Stored as `paste_as_text` and `paste_as_picture` in the config: a
comma list of item ids in menu order, `-` before an id hides it (`"lower,upper,-title"`); empty or missing is
the built-in list, unknown ids are ignored, and items added in later versions appear at the end, shown. Text
ids: `upper`, `lower`, `title`, `trim`, `collapse`, `oneline`, `json_pretty`, `json_minify`, `url_encode`,
`url_decode`, `base64_encode`, `base64_decode`, `layout` (keyboard layout), `translate`, `once` (Paste once). Picture ids:
`pic_text`, `pic_changed`, `pic_translate`, `pic_paste`, `once`.

**Paste with formatting.** ClipItTo keeps plain text, which is what is pasted by default. When a copy also has
formatting (bold, italics, links, lists, tables, headings, pictures in the page), the formatted (HTML) version is kept
next to its text, and *Shift+Enter › With formatting* (also in the right-click *Paste as* menu) pastes it as it was,
with the plain text alongside for programs that take only that. Copies whose HTML only wraps plain text (what a
browser adds to everything) keep nothing extra, and the item is not offered for them. Settings › *Keep formatting of
copied text* (`keep_formatting`) turns it off and deletes the formatted versions kept so far. Copies marked by a
password manager never keep one. Editing a copy's text drops its formatted version. Not included in a backup.
On GNOME (Wayland) it needs the updated extension (log out and in once after the update).

**Colours and files in the list.** A copy that is one colour (`#ff8800`, `#f80`, `#ff880080`, `rgb(255, 136, 0)`,
`rgba(0, 0, 0, 0.5)`) shows a swatch of it before the text. A copy that is a list of files (one path per line, or
`file://` links) shows a file icon and, for several files, the names (`report.pdf, notes.txt, +1 more`); a single path is shown in
full (cut in the middle if long), since it may be just text copied from a chat or a terminal. The paths are still the text, so
search and paste work on them. Files copied in a file manager that gives no text (Windows Explorer, macOS Finder) are
recorded as their paths, which is what pasting them into a text box gives. Only paths and colours are looked at: the
files are never opened or read, and the disk is not touched to draw the list.

**QR code.** Right-click a text copy › *Show as QR code…* shows it as a QR code you scan with your phone's camera:
a link to open there, Wi-Fi details (`WIFI:T:WPA;S:name;P:password;;`), a short note. It is drawn on this computer
and goes nowhere else. About 2,900 characters fit at most; a longer copy says so. Not offered for password copies
(their text is never put on the screen) or pictures. A code is on the screen like any text: a screenshot or screen
sharing can capture it, so close it when done (Esc), especially for Wi-Fi details.

**Search inside pictures.** ClipItTo reads the text in each copied picture in the background (on your computer,
with the same recognition as *Paste text from picture*) and keeps it with the picture, so typing a word finds the
screenshot that shows it, together with `img:`. Pictures copied before are read a few seconds after the start.
Turn it off with Settings › *Search inside pictures* (`index_pictures = false`): the text that was read is deleted.
If recognition is not available (for example no language data on Linux) the reading stops quietly.

**Translation.** Shift+Enter › *Translate* translates a copy, or the text in a picture, with a language pack you
add in Settings › Translation. It runs on your computer only; see [Translation](translation.md).
To read a translation without pasting it, press **Ctrl+T** on a copy, or choose *Show the translation only* in the *Translate*
submenu; the result opens in a window and is pasted only if you choose *Paste* there.

**Search filters.** Type these next to your words to narrow the list (a filter alone works too):

| Filter | Shows |
|---|---|
| `img:` | pictures only |
| `text:` | text copies only (no pictures, no passwords) |
| `pin:` / `star:` | pinned / starred copies |
| `group:work` | copies in a group whose name contains "work" (`group:` alone: in any group) |
| `note:todo` | copies whose note contains "todo" (`note:` alone: has a note) |
| `today:` | copied or used today |
| `since:3d` | copied or used in the last `30m`, `2h`, `3d`, `2w`, or since `yesterday` or a date like `2026-10-01` |

Example: `star: budget since:1w`. Words and filters are all required. Anything else with a colon (a URL,
`foo:bar`) is searched as ordinary text.

**Groups** sit collapsed right under the search box. Open them to browse your groups as a
tree: each group lists its sub-groups, then its copies. Put a copy in a group with `Ctrl+G`
and a path such as `Work/Servers` (missing groups are created). Groups appear in the order
you give them (`Alt+↑/↓`, or Move up/down in Settings); new ones go last.

**Auto grouping** (Settings › *Groups*, off by default; `auto_group`). ClipItTo learns from the copies you put in
your groups, and files a *new* copy in the group it matches best, with a small **auto** tag on its group chip. It
uses only words and shapes of the text (a link's site and first folder, a path's folders, a command's first word),
locally: no model, no network, and what it learns (hashed words, never the copies) stays in the history file, encrypted with it when history encryption is on; deleting one copy does not unlearn it, *Forget what was learned* does. It never creates a group,
never files password copies or pictures, and leaves a copy in Recent when no group clearly matches (a copy matches
one group, the best). Moving an auto-filed copy to another group, or out of its group, teaches it the other way, and
counts more than a first example. Turning it off keeps the copies already filed; *Forget what was learned* clears the
examples (Settings › Groups shows how many each group has). Copies already in Recent are not filed, only new ones.

**Stars** (`Ctrl+S`): a starred copy that is in no group is kept in **★ Favorites**, which is
always the first group (up to 10 copies; Settings allows up to 20). A starred copy in a group
is listed first in that group, up to 10 per group. Moving a starred copy into a group, or out
of one, takes its star along; ClipItTo tells you when the target is full.

**Paste keys per program.** ClipItTo pastes by typing keys into the program you came from, and terminals do not
paste with Ctrl+V. With *Choose the paste keys by the program* (Settings › General, `paste_keys_per_app`, on by default) it
looks at which program had the focus when the popup opened and types **Ctrl+Shift+V** for the well-known terminals
(Windows Terminal, GNOME Terminal, Konsole, xterm, Alacritty, kitty, WezTerm, Tilix, Terminator, foot, Ghostty, and more),
**Shift+Insert** for Windows consoles (cmd, PowerShell in the classic console, mintty), and `paste_keys` (Ctrl+V unless you
changed it) for everything else. *Programs* in the same place lists your own rules, up to 16, tried before the built-in
ones: a program name, which is its executable name (Windows, `.exe` optional), its window class (X11, `WM_CLASS`) or its
app id (GNOME), in any letter case with `*` for any text, and one of `ctrl+v`, `shift+insert`, `ctrl+shift+v`. In the
config file: `paste_keys_apps = ["Code = ctrl+shift+v", "xterm = shift+insert"]`. A rule can also turn a terminal back to
`ctrl+v`. To find a program's name on X11 run `xprop WM_CLASS` and click its window. ClipItTo only reads these names
(never a window title) and, with the diagnostic log on, writes which rule decided, not the program.
Where it works: **Windows** (executable name and window class), **X11** (`WM_CLASS`), **GNOME Wayland** with the ClipItTo
extension (log out and back in once after updating it; an older extension cannot say, then `paste_keys` is used).
Where it cannot: **macOS** always pastes with Cmd+V (terminals included, so there is nothing to choose), and **other Wayland
desktops** (KDE, wlroots) let no program ask which window has the focus; there `paste_keys` is used for every program and
auto-paste itself needs the GNOME extension anyway. Program names are matched whole (`xterm` does not match `xterm-256color`), a program can name its own window class, and the names in the config file are not secret. A terminal inside another program (VS Code's) is the program: add a
rule for it.

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
  its sub-groups, don't appear in Recent. The Passwords group has this as the *Show passwords entries in
  Recent* setting.

**Duplicates:** each text is one copy; copying it again moves it to the top. With *Ignore letter
case* (General, `ignore_case`, off by default) "ABC" and "abc" are the same copy and the wording you
copied last is the one kept; turning it on merges the copies already there (notes, groups and stars
move to the one that stays). A password is one copy too, whatever its note; with *Keep the same
password separately when its note differs* (Passwords, `passwords_per_note`, on by default) the
same password under another note (say, copied from another app) is its own copy, and only the same
password with the same note is merged.

**Generated passwords:** a copy from a browser's "new password" field has no marker, so it is plain
text. *Keep copies that look like generated passwords as password copies* (Passwords,
`detect_passwords`, off by default; needs a master password or *Use without one…*) keeps a
single-line copy of 8 to 64 characters without spaces that mixes three of lower case, upper case,
digits and symbols as a password copy. It is a guess by shape and will take some ordinary copies
(*Not a password* in the right-click menu undoes it); see [passwords.md](passwords.md).

**Your own password rules:** Settings › Passwords › *Password rules* (`password_rules`, up to 5)
keeps a copy as a password copy when it fits a rule you define: a length range, minimum counts of
lower case, upper case, digits and symbols ("at least 2 digits", not "only 2 digits"), and
optionally *digits at the end*. Rules add to the detection above. The *Try it* box tests a sample
(not saved or logged). Needs a master password or *Use without one…*, like the checkbox above.

**Auto-forget:** Settings › Passwords › *Auto-forget* (`auto_forget`, up to 8 rules, none by default) makes copies
remove themselves, quietly (no message; the log gets only a count). A rule is one of: *Older than N days* (since the
copy was last used; any copy, text only or pictures only), *Text containing …* (any letter case, at least 3
characters) removed N minutes after the copy, or *Looks like a secret* (an access token, a key, a card number, or
any of them) removed N minutes after the copy. It runs at start, once a minute, when the popup opens and when you
save the rules. Pinned, starred, grouped and password copies are never removed, and a password copy's text is never
looked at. Matching by the program a copy came from is not possible yet. In the file the rules are strings:
`auto_forget = ["age 30d", "age 1d picture", "match 5m contains \"Bearer \"", "match 10m token"]`. Removed copies are
deleted for good, like *Clear history* (nothing is left in the history file).

**Paste once.** For a one-time code, or a password you type into a stranger's form: a copy marked *Paste once* is removed
from the history right after you paste it from ClipItTo, and taken off the system clipboard if it still holds it.
- *Mark it* with *Paste once* in the **Paste as** menu (Shift+Enter, or the right-click › *Paste as* submenu; a toggle: choose it
  again to unmark). It is an item of the Paste as list like the others, so Settings › *Paste as* decides whether it is shown in the
  menu, under *Others*, or not at all, and where it comes first, last or between (id `once`; at the end by default). A password
  copy has no Paste as menu: its right-click menu has *Paste once* at the top level. The
  row then wears a small **once** tag. Or press `Ctrl+Shift+Enter` on any copy: it is pasted and forgotten in one go,
  marked or not. Undo shortcut: `Ctrl+Shift+Z`.
- A copy is stored like any other (encrypted with the history when that is on) and survives a restart until it is used.
  It is never in a backup (export leaves it out, import ignores it) and never found by search once used, because it is deleted:
  like *Clear history*, with nothing left in the history file.
- It is deleted only after the paste keys were really sent. If they were not (*Paste on Enter* is off, the session cannot
  type them, a key was blocked) the copy stays marked and the popup says so; the clipboard is left alone so you can paste by hand.
  ClipItTo only knows its own paste: a Ctrl+V you press later does not count, so the clipboard is emptied a few seconds after
  ClipItTo's paste (Settings › Passwords › *Clear a Paste once copy after*, `paste_once_clear_seconds`, 2 s by default, 0 = never,
  up to 60), only if it still holds that paste; something you copied meanwhile stays. The clearing is not stored as a copy and
  a clear still waiting when ClipItTo quits is done at once.
- **Password copies:** it pastes first, then asks *Delete this password copy?* **every time**; *No* keeps it (and it is not
  marked any more). A password copy is never deleted without that answer.
- **Pinned and starred copies are not deleted:** the mark is just removed and the popup says so quietly. Copies in a group
  (not starred) and pictures are deleted.
- **Undo:** for 10 seconds after a deletion the popup shows *Pasted and forgotten* with an **Undo** button (or `Ctrl+Shift+Z`)
  that puts the copy back as it was (text or picture, group, note, times; a password copy as its sealed copy), without the mark.
- The *Paste as* transforms and *With formatting* do not use up a marked copy; only pasting the copy itself does.

**A pasted password leaves the clipboard:** Settings › Passwords › *Clear a pasted password after* (`password_clear_seconds`,
2 seconds by default, 0 = never, up to 60) empties the clipboard that long after you paste a password copy from
ClipItTo, but only if the clipboard still holds that password: if you copied something else meanwhile it stays. The clear
is not stored as a copy, and a clear still waiting when ClipItTo quits is done at once.

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

**Text from a picture:** select a picture and press Shift+Enter (or right-click it › *Paste as*) for its choices: *Paste text from picture* first, *Text from picture, changed* (on one line, trimmed, collapsed spaces, upper / lower / Title case), and *Paste the picture*. *Paste text from picture* reads the text in it and puts it
on the clipboard and into the window you came from. It runs on your computer only, with no network or cloud
service: Windows uses its built-in text recognition (languages installed in Settings › Time & language), macOS
uses the Vision framework, and Linux uses Tesseract (the `.deb` recommends `tesseract-ocr-eng`; the AppImage
and snap carry English). For other languages on Linux install `tesseract-ocr-<language>` (for Hebrew,
`tesseract-ocr-heb`; ClipItTo adds the language of your locale to English), or put `*.traineddata` files in
`~/.local/share/clipitto/tessdata`. Choose the languages in Settings › General › *Read pictures in* (`ocr_languages`, e.g. `en, he`; empty is
automatic: Windows uses the languages installed for your account and the first you list that it has, macOS
detects the language by itself, Linux reads English plus your locale's language). A language that isn't
installed is reported by name. Where text recognition isn't available the entry is greyed out and says why.

**Pictures:** copied images (screenshots, "Copy Image" in a browser, ...) are kept too, shown
as a thumbnail with their size; hover a row for a larger preview, and choose it to put the
picture back on the clipboard. A copy that has both text and a picture is kept as text.
Pictures have their own limit, *Pictures kept in history* in Settings (`max_images`, default
100 unsaved pictures; 0 records none), and pictures larger than 50 MB (`max_image_bytes`) are
skipped. They are stored, and encrypted when that's on, in the same history file. Search
finds them by note, group or "image".

**Settings** (⚙): the options are explained in the dialog's tooltips and in
[config.example.toml](../data/config.example.toml). The ones worth knowing about:

- **Shortcut:** default Ctrl+Alt+V (Ctrl+` works too). On Windows and macOS ClipItTo registers
  it; on GNOME it becomes the GNOME keyboard shortcut, set again at every start; on KDE, Sway and
  other desktops bind `clipitto --toggle` yourself (`clipitto --status` reminds you).
- **Start on:** where the selection starts when the popup opens and after a search (the first
  pinned copy, the first copy in a group, or the first unsaved one).
- **Popup position:** near the pointer, centered, a corner, or where it was last (drag it by its name).
  **Wayland:** applications cannot place their own windows there. On GNOME the ClipItTo extension does it for
  the popup (all positions, and "where it was last" while the session lasts) and centers Settings over the popup;
  log out and back in after an update so the shell loads the new extension. On KDE, Sway and other Wayland
  desktops the desktop decides, and the setting has no effect.
- **Groups:** new, sub-group, rename/move, delete, move up/down; saved immediately.
- **History encryption**, **Backup** and [password copies](passwords.md#password-copies) have
  their own sections.

Saving keeps your own comments and layout in `config.toml`.

## When something does not work: the diagnostic log

`clipitto --status` shows the daemon's state in a few lines (the password feature has its own `password_feature` line:
which settings are on, whether the key that recognises copied passwords is available, and what the last "name this
password" box did).

For the reason behind a missing box or a copy that was not kept, turn the diagnostic log on. Normally ClipItTo writes almost
nothing to a log; with the log on it writes what it does (its debug lines), each with the time, to a file you can send.

- **In Settings:** tick *Write a diagnostic log (for troubleshooting)* (`debug_log = true` in `config.toml`), repeat what went
  wrong, and send the file. Unticking it stops the log and **deletes** the file. No restart is needed.
- **From a command line**, for one run, including the start-up: `clipitto-daemon --verbose` (on Windows quit ClipItTo first, then in
  PowerShell `& "C:\Program Files\ClipItTo\bin\clipitto-daemon.exe" --verbose`, with the path of your installation; on macOS
  `/Applications/ClipItTo.app/Contents/MacOS/clipitto-daemon --verbose`). The lines also appear in the terminal on Linux and macOS.

The file is `debug.log` in the same folder as the history: `%LOCALAPPDATA%\clipitto\` on Windows,
`~/.local/share/clipitto/` on Linux, `~/Library/Application Support/clipitto/` on macOS.

The file is created readable by you only (Linux and macOS; on Windows your user folder's permissions apply, as for the history), a
symbolic link at its path is refused, and it is emptied each time the log is switched on. At 2 MB it becomes `debug.log.1` and a new
`debug.log` starts, so the newest lines are always there (at most 4 MB in all). It does **not** contain what you copied (text,
pictures, notes, group names). It **does** contain file paths (with your user name), error messages and the times things happened,
for example when a copy that looked like a password was not taken, so look at it before sending it to anyone. Unticking the
setting deletes both files; quitting ClipItTo leaves them, and `--verbose` never deletes them.
