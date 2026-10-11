-- Pinned copies (listed at the top of the popup, in pin_position order) and a user-chosen
-- order for groups among their siblings.

-- A pinned copy is saved too: never pruned, ranked first in search. A generated column
-- can't be altered, so is_saved is dropped and added back with the new rule.
DROP INDEX idx_clips_saved_lastused;
ALTER TABLE clips DROP COLUMN is_saved;
ALTER TABLE clips ADD COLUMN pin_position INTEGER NULL;
ALTER TABLE clips ADD COLUMN is_saved INTEGER GENERATED ALWAYS AS
    (is_favorite = 1 OR (group_name IS NOT NULL AND group_name <> '') OR pin_position IS NOT NULL) VIRTUAL;
CREATE INDEX idx_clips_saved_lastused ON clips(is_saved, last_used DESC);
CREATE INDEX idx_clips_pinned ON clips(pin_position) WHERE pin_position IS NOT NULL;

-- Position among the groups with the same parent; NULL sorts after the ordered ones, by name.
ALTER TABLE groups ADD COLUMN position INTEGER NULL;
