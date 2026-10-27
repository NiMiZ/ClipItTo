#pragma once

#include "Config.h"

#include <QString>

#include <optional>

namespace clipitto {

class PasswordKeeper;

namespace db {
class Repository;
}

// Export and import of the history (copies, groups, stars, pins, notes, pictures, password
// copies) and/or the settings, as one JSON file.
//
// Copies and settings are plain text in the file. Password copies never are: each is encrypted
// under a key derived (Argon2id) from the master password given at export, with a random salt
// kept in the file. Nothing in the file depends on the machine (its keyring, vault or history
// key), so it opens anywhere with that password.
namespace backup {

struct Parts {
    bool copies = false; // the history, groups included
    bool settings = false;
    bool any() const { return copies || settings; }
};

// What a file holds.
struct Info {
    bool valid = false;
    QString error; // when not valid
    Parts parts;
    int copies = 0; // plain copies and pictures
    int passwords = 0; // encrypted password copies
    int groups = 0;
};

// Writes `parts` to `path` (atomically). Password copies need `keeper` unlocked and
// `password` (the master password's bytes); without any password copies neither is used.
bool exportTo(const QString& path, db::Repository& repo, const Config& config, PasswordKeeper* keeper, Parts parts,
              const QByteArray& password, QString* error);

Info inspect(const QString& path);
// Whether `password` opens the file's password copies (true when it has none).
bool passwordOpens(const QString& path, const QByteArray& password);

struct Report {
    int added = 0; // copies and pictures
    int alreadyThere = 0; // left as they are
    int passwordsAdded = 0;
    int passwordsSkipped = 0; // no master password here, or the backup's password is wrong
    std::optional<Config> settings; // when asked for: the file's settings, to be applied by the caller
};

// Merges the chosen `parts` of the file into the history. A copy that is already there is
// left alone. Password copies are re-sealed to this machine's vault: they need a master
// password set here (`keeper`) and the backup's `password`.
std::optional<Report> importFrom(const QString& path, Parts parts, db::Repository& repo, PasswordKeeper* keeper,
                                 const QByteArray& password, QString* error);

} // namespace backup
} // namespace clipitto
