-- Which filed copies the Recent list shows.
--  * groups.hide_from_recent: copies in that group (and its sub-groups) stay out of Recent.
--  * clips.off_recent: a filed copy the user took off Recent (Delete there, or Clear); it stays
--    in its group, and comes back when it is copied again. Copies in no group, star or pin
--    are always in Recent: this flag means nothing for them.
--  * meta: small settings kept with the history ('favorites_hide_from_recent' = '1' keeps
--    the starred copies of Favorites out of Recent).
ALTER TABLE groups ADD COLUMN hide_from_recent INTEGER NOT NULL DEFAULT 0 CHECK (hide_from_recent IN (0, 1));
ALTER TABLE clips ADD COLUMN off_recent INTEGER NOT NULL DEFAULT 0 CHECK (off_recent IN (0, 1));

CREATE TABLE meta (
    key    TEXT PRIMARY KEY,
    value  TEXT NOT NULL
);
