-- Paste once (roadmap 17): a copy marked to be removed from the history right after it is pasted from
-- ClipItTo. Stored like any other copy (encrypted with the history when that is on); the flag is cleared
-- by "Paste once" again, by an Undo, and when the copy is kept because it is pinned or starred.
-- Never exported to a backup.
ALTER TABLE clips ADD COLUMN paste_once INTEGER NOT NULL DEFAULT 0 CHECK (paste_once IN (0, 1));
