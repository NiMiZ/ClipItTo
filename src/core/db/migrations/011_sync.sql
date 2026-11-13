-- Sync through a shared folder (roadmap 13, docs/sync-design.md).
-- meta_changed: when pin, star, group or note of a copy last changed (NULL: never filed). The trigger keeps it,
-- unless a sync merge writes it itself (meta 'sync_applying' = 1).
ALTER TABLE clips ADD COLUMN meta_changed TIMESTAMP NULL;

-- Content hashes of copies that left this history, and when. Only 'shared' ones (a deletion the user asked for) are
-- written to this device's sync file; the others (pruned, forgotten, removed by a merge) only keep this PC from taking
-- the copy back from another device's file.
CREATE TABLE sync_tombstones (
    content_hash  TEXT PRIMARY KEY,
    deleted_at    TIMESTAMP NOT NULL,
    shared        INTEGER NOT NULL DEFAULT 0 CHECK (shared IN (0, 1))
);

CREATE TRIGGER clips_meta AFTER UPDATE OF tag_or_note, group_name, is_favorite, pin_position ON clips
WHEN coalesce((SELECT value FROM meta WHERE key = 'sync_applying'), '0') <> '1'
BEGIN
    UPDATE clips SET meta_changed = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = new.id;
END;

-- Recorded only while sync is on (meta 'sync_on'), never for password copies or Paste once copies.
CREATE TRIGGER clips_tombstone AFTER DELETE ON clips
WHEN old.secret = 0 AND old.paste_once = 0 AND coalesce((SELECT value FROM meta WHERE key = 'sync_on'), '0') = '1'
BEGIN
    INSERT OR REPLACE INTO sync_tombstones(content_hash, deleted_at, shared)
    VALUES (old.content_hash, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
            coalesce((SELECT value FROM meta WHERE key = 'sync_share_deletes'), '0') = '1');
END;
