# Snapshots (roadmap 29): design

A **snapshot** is a named set of text copies ("project X") that the user makes on purpose and can put back into the
history later. It holds **its own copies**, so it survives pruning, auto-forget and *Clear*.

## What goes in

- Made from **one copy** (right-click › *Save as snapshot…* › *New snapshot with this copy…*, or *Add to* an existing one)
  or from **a whole group** (group menu › *Save group as snapshot…*; the group and its sub-groups, in the order the popup
  lists them). The popup has single selection, so there is no "several selected copies"; a group is the way to take many.
- **Plain text copies only.** Skipped, and **counted in the message** ("Saved 12 copies. Skipped: 2 passwords, 1 picture"):
  - password copies and anything marked as a password (`secret`): never read, never stored. This is the hard rule;
  - pictures (a snapshot of pictures would mean copying PNG blobs: not small, not in this version);
  - *Paste once* copies (they are meant to vanish);
  - copies over 1 MB of UTF-8 text, and copies beyond the limit below.
- The note of a copy comes along (it is part of how people tell copies apart). Formatting (HTML) does not.
- The same text twice in one snapshot is kept once.
- Limits (constants in `Repository.h`): **50 snapshots, 500 copies each, 1 MB per copy**, name 1 to 80 characters, names
  unique (not case-sensitive). A refused action says which limit and changes nothing.

## Where it is stored (encryption)

Inside the same SQLite history file, in two new tables (migration 012: `snapshots`, `snapshot_items`). So **whatever
protects the history protects the snapshots**: when `encrypt_history` is on the whole file is encrypted (including these
tables, also when the user switches encryption on or off: that rekeys the whole file); no separate file, no clear text
beside an encrypted history. The tables have no triggers and no search index, so search never finds snapshot text and the
search index never holds it. Deleting a snapshot and the wipes below zero the pages (`secure_delete`) and fold the
write-ahead log in.

## Restoring

Window › *Restore*, with two choices (remembered only while the window is open):
- **Order:** *as copied* (the copies keep their relative time order: the one copied last ends on top) or *as saved*
  (the first copy in the snapshot ends on top, which is the order the group showed).
- **Put them in a group named after the snapshot** (off by default). Only copies that were added by the restore, and
  existing ones that were in no group, join it; a copy already in another group stays where it is. A star limit that
  refuses a filing is ignored for that copy (it stays unfiled).

Copies are **deduped against the history** (same rule as capture: `Repository::ingest`, so the ignore-case choice
applies): an existing copy is not duplicated, it is only moved to the top (this is what makes the order work). The message
says how many were added and how many were already there. Without a group, restored copies are ordinary unsaved
copies and can be pruned again as `max_history` says; a restore of a snapshot larger than `max_history` into no group
therefore loses its oldest copies at the next prune; use the group option to keep them.

## Management UI

`SnapshotsWindow` (popup: *Clear* button's menu › *Snapshots…*, group menu, copy menu): a list (name, number of copies,
date), *Restore*, *Rename…*, *Delete…* (asks first), with the two restore choices. Names and counts only: the window
never shows the text of a copy. It is closed and forgotten by the panic action.

## Not done (and why)

- **Paste one after another**: not offered. The paste queue (roadmap 4) does not exist on main, and a queue is not a
  small, safe addition (it needs a global paste hook and state across focus changes).
- **Backup / export**: snapshots are **not** in backups. The backup is a plain JSON file whose design carries only the
  history, and the encryption of the history file does not extend to it; putting snapshot text there would create a
  clear-text copy beside an encrypted history. Documented in usage.md.
- **Sync**: snapshots are never synced (the sync file is built from `clips` only).
- **`--status` and logs**: nothing about snapshots in `--status`; the debug log has counts only, never names or text.

## Wipes

`Repository::clearHistory(everything, alsoSnapshots)`: *Clear Recent* and *Clear everything, also in groups* in the popup
**keep** snapshots (they are the thing the user keeps on purpose; the confirmation says so when there are some). The
panic action and *Wipe everything now…* (`Daemon::wipeHistory`) pass `alsoSnapshots = true`: the same transaction deletes
every snapshot, then the usual search-index merge and WAL checkpoint run. The panic note in
`.claude/notes/passwords.md` (not edited here) should gain "and snapshots". The snapshots window is closed on panic.
