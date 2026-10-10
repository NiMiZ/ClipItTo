-- Initial schema. Applied by Migrator inside a transaction; user_version is set by Migrator.

CREATE TABLE clips (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    content       TEXT    NOT NULL,
    content_hash  TEXT    NOT NULL UNIQUE,              -- SHA-256 hex of exact UTF-8 content
    created_at    TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    last_used     TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    is_favorite   BOOLEAN NOT NULL DEFAULT 0 CHECK (is_favorite IN (0, 1)),
    group_name    TEXT NULL,
    tag_or_note   TEXT NULL,
    is_saved      INTEGER GENERATED ALWAYS AS
                  (is_favorite = 1 OR (group_name IS NOT NULL AND group_name <> '')) VIRTUAL
);

-- Recent list (unsaved, newest first) and prune scan (unsaved, oldest first) share this index.
CREATE INDEX idx_clips_saved_lastused ON clips(is_saved, last_used DESC);
CREATE INDEX idx_clips_group ON clips(group_name) WHERE group_name IS NOT NULL;

-- Substring search across note, group and content.
CREATE VIRTUAL TABLE clips_fts USING fts5(
    tag_or_note, group_name, content,
    content = 'clips', content_rowid = 'id',
    tokenize = 'trigram case_sensitive 0'
);

CREATE TRIGGER clips_ai AFTER INSERT ON clips BEGIN
    INSERT INTO clips_fts(rowid, tag_or_note, group_name, content)
    VALUES (new.id, new.tag_or_note, new.group_name, new.content);
END;

CREATE TRIGGER clips_ad AFTER DELETE ON clips BEGIN
    INSERT INTO clips_fts(clips_fts, rowid, tag_or_note, group_name, content)
    VALUES ('delete', old.id, old.tag_or_note, old.group_name, old.content);
END;

-- last_used-only updates (dedupe bumps) deliberately do not touch the FTS index.
CREATE TRIGGER clips_au AFTER UPDATE OF content, tag_or_note, group_name ON clips BEGIN
    INSERT INTO clips_fts(clips_fts, rowid, tag_or_note, group_name, content)
    VALUES ('delete', old.id, old.tag_or_note, old.group_name, old.content);
    INSERT INTO clips_fts(rowid, tag_or_note, group_name, content)
    VALUES (new.id, new.tag_or_note, new.group_name, new.content);
END;
