# Using ClipItTo

Back to the [README](../README.md).

| Key | Action |
|---|---|
| type | filter history (note, group and text) |
| `↓` / `↑` | move the selection (it starts on the latest copy: `↓` next copy, `↑` the Groups row); `↑` on the top row returns to the search bar |
| `Shift+Enter` | **Paste as…** a menu of changes to the text for this paste only: UPPER, lower, Title Case, trim, collapse spaces, one line, JSON pretty / minified, URL and Base64 encode / decode (press the underlined letter). The stored copy is not changed |
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

**Duplicates:** each text is one copy; copying it again moves it to the top. With *Ignore letter
case* (General, `ignore_case`, off by default) "ABC" and "abc" are the same copy and the wording you
copied last is the one kept; turning it on merges the copies already there (notes, groups and stars
move to the one that stays). A password is one copy too, whatever its note; with *Keep the same
password separately when its note differs* (Passwords, `passwords_per_note`, off by default) the
same password under another note (say, copied from another app) is its own copy, and only the same
password with the same note is merged.

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

**Settings** (⚙): the options are explained in the dialog's tooltips and in
[config.example.toml](../data/config.example.toml). The ones worth knowing about:

- **Shortcut:** default Ctrl+Alt+V (Ctrl+` works too). On Windows and macOS ClipItTo registers
  it; on GNOME it becomes the GNOME keyboard shortcut, set again at every start; on KDE, Sway and
  other desktops bind `clipitto --toggle` yourself (`clipitto --status` reminds you).
- **Start on:** where the selection starts when the popup opens and after a search (the first
  pinned copy, the first copy in a group, or the first unsaved one).
- **Popup position:** near the pointer, centered, a corner, or where it was last (drag it by its name).
- **Groups:** new, sub-group, rename/move, delete, move up/down; saved immediately.
- **History encryption**, **Backup** and [password copies](passwords.md#password-copies) have
  their own sections.

Saving keeps your own comments and layout in `config.toml`.
