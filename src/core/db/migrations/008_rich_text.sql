-- The HTML flavour of a text copy that has formatting (bold, links, lists, tables...), kept next to its plain text
-- so it can be pasted with its formatting. One row per such copy; it goes with the copy (ON DELETE CASCADE), is not
-- in the search index, and is dropped when the copy's text is edited or becomes a password.

CREATE TABLE clip_rich (
    clip_id INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE,
    html    TEXT    NOT NULL
);
