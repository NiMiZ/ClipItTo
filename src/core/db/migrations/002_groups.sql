-- Groups as a tree of paths ("Work/Servers"; "/" separates levels). Rows exist for every
-- level, so a group can be empty and its parents always exist. clips.group_name holds the
-- path of the (single) group a clip belongs to.

CREATE TABLE groups (
    path        TEXT NOT NULL PRIMARY KEY,
    created_at  TIMESTAMP NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

-- Adopt groups already used by clips, with all their ancestors.
-- rtrim(path, <all non-slash characters of path>) strips the last segment, leaving "parent/".
WITH RECURSIVE ancestry(path) AS (
    SELECT DISTINCT group_name FROM clips WHERE group_name IS NOT NULL AND group_name <> ''
    UNION
    SELECT substr(rtrim(path, replace(path, '/', '')), 1,
                  length(rtrim(path, replace(path, '/', ''))) - 1)
    FROM ancestry WHERE instr(path, '/') > 0
)
INSERT OR IGNORE INTO groups(path) SELECT path FROM ancestry;
