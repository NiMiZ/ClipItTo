# Settings, groups, storage, backup (backend)

## Storage (`src/core/db`)
SQLite3 Multiple Ciphers (optional encryption, key in system keyring), WAL, `secure_delete`, FTS5
trigram. Migrations `src/core/db/migrations/001..006_*.sql`; each new one must be added to
`qt_add_resources` in `src/core/CMakeLists.txt` and `tst_migrator` expects the schema version (9 now).
- 006: `groups.hide_from_recent`, `clips.off_recent`, `meta(key,value)`.
- 005: `clips.secret`, `clip_secrets(sealed)`, `vault`. 004: images (content = label).
`Repository` (+ `Stmt`/`Db`/`Transaction` wrappers) is the only SQL user. Key methods:
`recent(limit, unsavedOnly)`, `recentUnpinned`, `takeOffRecent`, `countRecent`, `clearRecent`,
`setGroupHiddenFromRecent`, `setFavoritesHiddenFromRecent`, `allEntries`, `findByHash`,
`restoreFiling(id, created, lastUsed, note, group, favorite, offRecent)`, `offRecentIds`,
`placeGroupLast(path)`, `setIgnoreCase/ignoreCase/textHash`, `setSecretsPerNote`, `hasSecret(match,note)`, `findSecrets`.

- 009: auto grouping: `clips.auto_filed`, `auto_examples(group_name, examples)`, `auto_tokens(group_name, token hash, weight)`.

## Auto grouping (`AutoGroup.{h,cpp}`, roadmap 16, `auto_group`, off by default)
Tokens (hashed words + URL host/domain/folder, path folders, command word) -> `Model` snapshot (`Repository::autoModel`,
cached, reset by learn/forget/rename/delete). Learning is in `Repository::setGroup` only while `setAutoGrouping(true)`;
a move of an `auto_filed` copy is a correction (weight 3, old group -3). `CapturePipeline::autoFile` classifies on a
worker (QtConcurrent) and `Repository::setAutoGroup` files it (unsaved, existing group, never creates one) -> signal
`autoFiled`. Never for secrets/pictures. Seeds once from copies already in groups (`meta.auto_seeded`; Forget sets it).

## Groups and stars
Path syntax `Work/Servers` (missing groups created, `GroupPath`). Groups ordered manually (Alt+Up/Down,
new ones last). Star = Favorites (limit `favorites_limit`, 1-20) when ungrouped; in a group up to 10, listed
first; moving into/out of a group carries the star. Pinned max 10 (`saved_cap` shows 15). Pinned/starred/
grouped are never pruned (`max_history` prunes unsaved; `max_images` separate).

## Duplicates
One copy per `content_hash`; re-copy bumps it. `ignore_case` (off): hash of `toCaseFolded()`, newest wording
kept (ingest updates `content`); turning it on/off re-hashes in one transaction and merges duplicates into
the most recently used (note/group/star/pin moved over if survivor lacks them); mode in `meta('ignore_case')`.
Passwords: one copy per keyed match hash `secret:<hex>`; with `passwords_per_note` the hash is
`secret:<hex>:<random>` and `hasSecret(match, note)` / `findSecrets` find same-password-same-note.
Known caveat: rename a note, re-copy from source app => new copy.

## Config (`Config.{h,cpp}`, `data/config.example.toml`, `SettingsDialog`)
TOML via toml++, user comments/layout preserved on save. Hot reload with `clipitto --reload`
(`Daemon::reload` re-applies setIgnoreCase/setSecretsPerNote). Removed key: `passwords_in_recent`
(now a Passwords-group rule set in `SettingsDialog::accept`). New keys must go in Config.cpp load+save,
config.example.toml, SettingsDialog (init-list order must equal declaration order: -Werror=reorder in CI),
and tst_config/tst_settings. Settings tabs: General, ..., Passwords, Backup (index 3; tab count test = 4).

## Backup (`Backup.{h,cpp}`, Settings > Backup)
JSON `{format:"clipitto-backup", version:1}`: copies (text; images base64), pins, stars, groups (with order,
`hide_from_recent`), notes, `off_recent`, settings as TOML. Parts: copies / settings / both (import asks
when both). Master password (if set) is asked first for export and import. Password copies are boxed with a
key from Argon2id(64 MiB x3, salt in file) of the master password + "check" verifier -> machine-independent,
re-sealed to the importing PC's vault (it needs its own master password). Import skips copies already
present (`repo.textHash`, `hasSecret`), places new groups last in file order. Settings import keeps local
hotkey, backend, encrypt_history, windows_hello. File written owner-only via QSaveFile. Tests: `tst_backup`.
Only exercised on Linux; format is plain JSON so cross-OS should work.

## Build/CI facts
Presets: debug, asan, deb, release, windows, macos, macos-release. CI jobs: linux ASan -Werror, .deb, windows
MSVC, macos, appimage, snap, install checks, release (tags only). Lambdas with QVERIFY must have explicit
return type (MSVC/clang). Stacked PRs: a PR stacked on a branch that already merged lands in the dead branch.
