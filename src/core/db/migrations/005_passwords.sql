-- Password copies. The clips row holds a label ("Password") as its content, so notes, groups,
-- stars, pins, pruning and search by note or group work as for text; the password itself is
-- only ever stored sealed (PasswordVault), here, and goes with its row. content_hash is
-- "secret:<keyed hash>" (or a random "secret:unmatched:..." without the keyring's match key).
-- A new column, not a new kind: the kind CHECK can't change without rebuilding the table.

ALTER TABLE clips ADD COLUMN secret INTEGER NOT NULL DEFAULT 0 CHECK (secret IN (0, 1));

CREATE TABLE clip_secrets (
    clip_id  INTEGER PRIMARY KEY REFERENCES clips(id) ON DELETE CASCADE,
    sealed   BLOB    NOT NULL
);

-- The master password's vault: one row, or none until a master password is set. The master
-- password itself is never stored; it derives (Argon2id) the key that unwraps wrapped_secret.
CREATE TABLE vault (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    salt            BLOB    NOT NULL,
    kdf_blocks      INTEGER NOT NULL,
    kdf_passes      INTEGER NOT NULL,
    public_key      BLOB    NOT NULL,
    wrapped_secret  BLOB    NOT NULL,
    hello_wrapped   BLOB    NULL -- the secret key under a Windows Hello key; NULL when off
);
