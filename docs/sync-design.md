# Sync through a shared folder (roadmap 13): design

ClipItTo keeps the history of several of your PCs in step through **a folder you already sync** with
something else (Syncthing, Nextcloud, OneDrive, Dropbox, a network share). ClipItTo itself makes **no
network connection**: it only reads and writes ordinary files in that folder. Off by default.

## The rule everything follows

**The live database (`history.db`, with its WAL) is never put in the synced folder.** Two PCs writing one
SQLite file through a sync client corrupts it (two writers, half-copied pages, `-wal` and `-shm` files that
are copied at different moments). So the database stays in the local data directory, and the folder holds
only small, separate *sync files*:

```
<folder>/
  3f9c…e1.clipitto-sync     written only by device 3f9c…e1
  b72a…07.clipitto-sync     written only by device b72a…07
```

Each device writes **exactly one file, its own**, and never edits another device's file. A sync client
therefore never has a file with two writers, and a conflict copy ("file (conflicted copy)") cannot happen
unless the user copies the id of a device by hand. Everything else is derived by *reading* the other files.

## What is in a sync file

A snapshot of one device, replaced as a whole when something changed:

- **Copies**: text, and pictures (below a size cap), each with its metadata: pin, star, group, note,
  `created`, `last_used`, and `meta_changed` (when pin/star/group/note last changed on that device).
- **Groups** (path, "hide from Recent").
- **Tombstones**: the content hash of a copy the user deleted on this device, and when. Kept for
  `sync_tombstone_days` (default 30).
- Device id (random 128 bit, made once, kept in the history database), device name (a setting), time written,
  format version.

**Never in the file**: password copies (they are sealed to *each* device's vault and stay there), copies kept
by the secret scanner (keys, tokens, card numbers; they are password copies too), and *Paste once* copies.
The export code skips them by flag, and a test checks that a file written from a history full of them holds
none (and that an imported file containing one is ignored). The formatted (HTML) version of a copy is not
synced either (only the plain text); rich copies arrive as plain copies.

## Encryption

The folder is somebody else's storage (a cloud, a share), so the file is encrypted:

- The key comes from a **sync passphrase** set in Settings. It is *not* the master password (that one may be
  unset, or differ between PCs, and one should not have to share it).
- `key = Argon2id(passphrase, salt, 64 MiB, 3 passes)`, the same Monocypher routine as the backup
  (`PasswordVault::deriveBackupKey`), no new dependency. The derived key encrypts the file with
  XChaCha20-Poly1305 (`PasswordVault`, own context string `clipitto-sync-v1`, so a sync box cannot be passed
  off as a backup box or the other way round).
- The **salt is shared by all devices of a group**: every device needs the same key from the same passphrase.
  The first device makes a random salt; a device that joins reads it from a file already in the folder, derives
  the key and checks it by opening that file. A wrong passphrase fails right there, in the dialog.
- The passphrase is **never stored**, in the config or anywhere else. The derived key (with its salt) is kept
  in the **system keyring** (`KeyStore`, entry `sync-key`), so sync runs unattended in the background. Without
  a keyring sync cannot be turned on (it says so), because the alternative would be a key in a plain file.
- File layout: `CLIPSYNC` (8 bytes) | format version (1) | salt (16) | Argon2 memory KiB (4) | passes (4) |
  nonce (24) | MAC (16) | ciphertext. The plaintext is `qCompress`ed JSON. The reader checks the header,
  bounds the Argon2 numbers, the file size and the size announced for the decompressed text before it
  allocates anything.
- A file whose salt is not ours, or that does not open with our key, **is ignored** and Settings says how many
  and why ("made with a different sync passphrase", "damaged or still being copied"). A file that fails to
  open while its sync client is still writing it is simply tried again at the next run.
- Trade-off: anyone who has the folder and the passphrase can read the copies; anyone with only the folder
  cannot (they see the device ids, the file sizes and the times). Changing the passphrase makes a new salt:
  this device rewrites its file, and the other devices must be given the new passphrase (their files are
  ignored until then; Settings shows that).

## Writing the own file

1. Collect the snapshot: ids of the exported copies come from one cheap query; the contents are read in small
   batches on the GUI thread (one batch per event-loop turn), so the popup never waits.
2. Build the JSON, compress, encrypt and write **on a worker thread**.
3. Atomic: write to `<name>.tmp-<random>` in the **same folder**, flush, `fsync`, then rename over the real
   name (`std::filesystem::rename` replaces atomically on POSIX and with `MoveFileEx` on Windows), then
   `fsync` the folder on POSIX. A half file is never visible under the real name. A failed attempt deletes the
   temp file. Sync clients that lock the file on Windows (a sharing violation) are retried with a growing delay
   (100, 200, 400, 800 ms).
4. **Only when something changed**: a fingerprint (SHA-256 over the exported copies' hash, `last_used`,
   `meta_changed`, the tombstones and the sync settings) is compared with the one of the last write. A quiet
   history means no write, so a sync client sees no traffic.
5. **Size cap**: the newest `sync_max_copies` (default 2000) unsaved copies, plus *all* saved ones (pinned,
   starred, grouped; they are few). Pictures only when `sync_pictures` is on and under
   `sync_picture_max_kb`.

## Reading and merging

Triggers: a timer (about every 60 s), a `QFileSystemWatcher` on the folder (debounced, 3 s), after each
setting change, `clipitto --sync` and the *Sync now* button. Watching can fail on network shares, which is
why the timer always runs too.

Step 1, off the GUI thread: list `*.clipitto-sync` files, skip our own, skip files unchanged since the last
run (same size and modified time), read, decrypt, parse. This is the slow part (disk, network share,
decompressing, JSON) and it never touches the database.

Step 2, on the GUI thread, **in batches** (25 copies per event-loop turn, one transaction per batch): the
merge into the local database. This has to be on the thread that owns the connection, and batches keep every
turn short. (Tests guard the stall time with `StallWatchdog`.)

Merge rules, per copy, matched by content hash (a picture by its pixel hash):

| Situation | Result |
|---|---|
| Copy is new here | Added, with the other device's `created`, `last_used` and metadata. Not marked auto-filed. `last_used` is kept as it was, so the history order has no surprises (a synced old copy lands where it belongs, not at the top). |
| Copy exists here | `last_used` stays as it is here. If the other `meta_changed` is **newer** than ours, pin, star, group and note are taken **as a set** from the newer side (a partial mix could contradict itself). Pins and stars obey the caps (a full list refuses, the copy keeps its old state). |
| Tombstone, copy here | The copy is deleted (secure delete, search index merged, WAL folded in once at the end of the run) **unless** it is newer here than the tombstone (last used or changed after the deletion = the user used it again). |
| Tombstone, copy not here | Remembered; the copy is not re-added from another device's file. |
| Copy in a file and a newer local or foreign tombstone | Not added (deleted after it was last used there). |
| Unsaved copy older than all of ours and our history is at its limit | Not added (it would be pruned at once). |
| Password copy, "once" copy, an item that fails validation | Ignored. |

Everything the merge decides depends only on the file contents and the local state, so running it twice
changes nothing (idempotent), and two devices that read each other's files end up with the same set of
copies, pins and groups (convergence is tested with two simulated devices).

**Deleting locally** records a tombstone through a trigger (any way a copy leaves the database), but only
deletions the user asked for (the Delete action, Clear history, Clear Recent) are *shared* in the file.
Copies that leave through pruning (`max_history`), auto-forget or a merge are remembered **only locally**,
so this PC does not take them back from another PC's file, but they are not deleted elsewhere.

## Database changes (migration 011)

- `clips.meta_changed`: when pin, star, group or note last changed (NULL: never filed). A trigger stamps it on every
  such update unless `meta('sync_applying') = 1`, which only the merge sets (inside its own transaction) so it can
  write the other device's time.
- `sync_tombstones(content_hash, deleted_at, shared)`: filled by a trigger on any deletion while `meta('sync_on') = 1`
  (never for password or Paste once copies). `shared = 1` only when the deletion went through `remove()`,
  `clearRecent()` or `clearHistory()` (they set `meta('sync_share_deletes')` around the DELETE). Pruning, auto-forget
  and merge deletions are `shared = 0`: they stop this PC from taking the copy back, and are not announced.
- `meta`: `sync_on`, `sync_device_id`, `sync_fingerprint` (what the last written file was made from).

## Threads

The database belongs to the GUI thread (one connection, no locking). Off the GUI thread: listing, reading,
decrypting and parsing the other files, and encoding, encrypting and writing the own file (`QtConcurrent`), plus the
Argon2id work for a passphrase. On the GUI thread, in batches of 25 copies per event-loop turn (one transaction
each): the merge and the collection of the own copies. Measured with 3000 copies: the longest GUI stall is about 10 ms
(`tst_sync`, `tst_syncservice`, budget 100 ms, CI x5). The first version rewrote the search index for every merged copy
(an UPDATE that names the indexed columns), which grew a batch to 100 ms; unfiled copies are now written without
naming them.

## Settings and keys

`sync_enabled` (false), `sync_folder` (""), `sync_device_name` ("" = host name), `sync_max_copies` (2000, 100-20000),
`sync_pictures` (true), `sync_picture_max_kb` (2048, 64-20480), `sync_tombstone_days` (30, 1-365). The
passphrase is not a key. Settings gets a **Sync** tab: enable, folder (Browse…), passphrase Set/Change/Remove
(masked dialogs; the text is wiped after use), *Sync now*, status line (last sync, devices seen, files ignored
and why), device name, and a plain statement of what is and is not synced. `clipitto --sync` runs one sync.

## Failure behaviour

- Folder missing or not writable (unplugged share, drive not mounted): the run ends with "folder not
  available", the local history is untouched, retry backs off 1, 2, 4, 8, … up to 15 minutes, and the
  next change of the folder (watcher), *Sync now* or the settings resets it.
- A file is locked by the sync client: retried with backoff; else the next run.
- Nothing is logged but the folder path and counts. No copy text ever reaches a log, a status line or an error.
- No symlinks are created; the code uses `std::filesystem`/`QDir` and closes every file before deleting or
  renaming it.

## Known limits (honest list)

- **Clocks.** "Newest wins" compares times written by different PCs. A PC whose clock is minutes off can win or
  lose a metadata conflict, or have a tombstone applied late/early. Keep clocks on network time.
- **Tombstones expire.** A PC that is off longer than `sync_tombstone_days` can bring a deleted copy back.
- **Ignore-case must be the same** on all devices (the hash differs otherwise; copies are then added twice
  rather than lost).
- **Pin order** is local. Only *whether* a copy is pinned is synced.
- **A device id cloned by copying the database** to another PC gives two writers for one file; the two PCs
  overwrite each other's file (neither loses local data, but each other's copies stop arriving). There is no
  automatic detection yet; a restored backup of the *settings* does not copy the id (it lives in the database).
- **Two groups made at the same time.** The salt is shared, so a PC that sets the passphrase while the folder holds no
  file yet makes a new salt. If two PCs do that before either has written, they have two groups and each ignores the
  other's file ("different sync passphrase"). Typing the same passphrase again on both (*Change…*) fixes it: joining
  tries every group in the folder and takes the one with the smallest salt, on every PC. The docs tell the user to
  finish the first PC before the second.
- **Deletion notices hold a hash.** A tombstone is the SHA-256 of the deleted text (or the pixel hash of a picture),
  kept in the local database for `sync_tombstone_days` and in the sync file (encrypted) as long. A short guessable text
  could be confirmed from its hash by someone holding the history file. Mitigations: copies that look like a key,
  token or card number leave no hash when deleted with the Delete action; turning sync off deletes every local notice;
  password copies and Paste once copies never leave one; the history file can be encrypted (Settings). *Clear
  history* does leave the hashes of what it cleared (it cannot know which ones looked secret without reading every
  text).
- The group sort order is not synced, only the paths and "hide from Recent".
- A sync file stays on the other PCs until they read it; a PC that is never started again keeps its last file.
  Remove a retired device's file by hand.
- Not tried with real sync clients on Windows or macOS (see `docs/untested.md`).

## Review findings kept as known limits

- **Rollback.** Someone who can write the folder can put an old copy of a device's file back (no key needed); deleted
  copies then return. A per-device "last written" check is not built yet.
- **Hostile group members / folder writers.** Times from a file are clamped to now + 5 minutes; keys and tokens in a
  file are not imported; names are trimmed. Not yet capped: the number of saved (pinned/starred/grouped) items one file
  may bring, the star caps on import, and pictures are not re-decoded or re-hashed. A file can make ClipItTo read up to
  256 MiB per poll; a stalled share can delay quitting (workers are not cancelled).
- **Keyring on the GUI thread.** Reading, writing and removing the key use the keyring from the GUI thread (like the
  history key), once per session unless it errors.
- **Wiping.** Key copies held by worker lambdas are not zeroed (Qt shares the buffers); the passphrase text from the
  dialog is wiped after use.

## Alternatives rejected

- *The database in the shared folder*: corrupts (above).
- *One shared file all devices edit*: needs locking over a sync client, which has none; guaranteed conflicts.
- *Event log per device* (append-only): smaller writes, but needs compaction and ordering across devices;
  a snapshot of at most a few MB is simpler, atomic and self-healing.
- *Using the master password*: may be unset, may differ per PC, and ties password safety to sync safety.
- *Key in the config*: the folder and the config often travel together (dotfile sync); a keyring is the one
  place nothing syncs.
