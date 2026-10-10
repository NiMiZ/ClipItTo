#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace clipitto::db {

class Db;

struct Migration {
    int version;
    QString name;
    QByteArray sql;
};

// Migrations compiled into the binary (:/clipitto/migrations/NNN_name.sql), sorted by version.
std::vector<Migration> builtinMigrations();

int schemaVersion(Db& db);

// Applies every migration newer than PRAGMA user_version, each in its own transaction.
// Throws DbError if the database was created by a newer schema than we know about.
void migrate(Db& db);
void migrate(Db& db, const std::vector<Migration>& migrations);

} // namespace clipitto::db
