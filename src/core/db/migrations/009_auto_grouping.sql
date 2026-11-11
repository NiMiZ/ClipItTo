-- Auto grouping (off by default; roadmap 16). What was learned from the copies the user put in
-- groups, kept in the history file (so it is encrypted with it):
--  * auto_examples: how many examples each group has (shown in Settings).
--  * auto_tokens: per group, how often a token (a 64-bit hash of a word or shape of a copy, never
--    the text itself) was seen in its examples.
--  * clips.auto_filed: the copy was put in its group automatically (the "auto" tag); any move
--    the user makes clears it.
ALTER TABLE clips ADD COLUMN auto_filed INTEGER NOT NULL DEFAULT 0 CHECK (auto_filed IN (0, 1));

CREATE TABLE auto_examples (
    group_name  TEXT    PRIMARY KEY,
    examples    INTEGER NOT NULL
);

CREATE TABLE auto_tokens (
    group_name  TEXT    NOT NULL,
    token       INTEGER NOT NULL,
    weight      INTEGER NOT NULL,
    PRIMARY KEY (group_name, token)
) WITHOUT ROWID;
