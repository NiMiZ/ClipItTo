-- The text read from a picture (OCR), so a search finds a screenshot by what it shows. One row per picture that
-- was read (an empty text means "read, nothing found", so it is not read again); it goes with its picture
-- (ON DELETE CASCADE) and is not in the search index: a search matches it with LIKE, among the pictures only.

CREATE TABLE clip_text (
    clip_id INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE,
    text    TEXT    NOT NULL
);
