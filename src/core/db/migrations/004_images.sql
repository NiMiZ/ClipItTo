-- Image copies. The clips row holds the label ("Image 1920×1080") as its content, so search,
-- notes, groups, stars, pins and pruning work as for text; the picture lives here and goes
-- with its row (ON DELETE CASCADE, enforced since foreign_keys = ON).

ALTER TABLE clips ADD COLUMN kind TEXT NOT NULL DEFAULT 'text' CHECK (kind IN ('text', 'image'));

CREATE TABLE clip_images (
    clip_id    INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE,
    png        BLOB    NOT NULL,
    thumbnail  BLOB    NOT NULL,
    width      INTEGER NOT NULL,
    height     INTEGER NOT NULL
);

-- Unsaved images, oldest first: the image prune scan.
CREATE INDEX idx_clips_kind_saved_lastused ON clips(kind, is_saved, last_used);
