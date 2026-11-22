-- Snapshots (roadmap 29, docs/snapshots-design.md): named sets of text copies that keep their own text.
-- They live in the history file, so they are encrypted with it. No triggers and no search index: search never sees them.
CREATE TABLE snapshots (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE COLLATE NOCASE,
    created_at  TIMESTAMP NOT NULL
);

CREATE TABLE snapshot_items (
    id           INTEGER PRIMARY KEY,
    snapshot_id  INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    position     INTEGER NOT NULL,
    content      TEXT NOT NULL,
    note         TEXT NULL,
    content_hash TEXT NOT NULL,
    copied_at    TIMESTAMP NOT NULL,
    UNIQUE (snapshot_id, content_hash)
);
CREATE INDEX snapshot_items_by_snapshot ON snapshot_items(snapshot_id, position);
