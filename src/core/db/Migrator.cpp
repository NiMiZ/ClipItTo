#include "db/Migrator.h"

#include "db/Sqlite.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>

namespace clipitto::db {

std::vector<Migration> builtinMigrations()
{
    static const QRegularExpression fileName(QStringLiteral(R"(^(\d+)_.*\.sql$)"));

    std::vector<Migration> result;
    const QDir dir(QStringLiteral(":/clipitto/migrations"));
    for (const QString& entry : dir.entryList({QStringLiteral("*.sql")}, QDir::Files)) {
        const auto match = fileName.match(entry);
        if (!match.hasMatch()) {
            continue;
        }
        QFile file(dir.filePath(entry));
        if (!file.open(QIODevice::ReadOnly)) {
            throw DbError("cannot read migration " + entry.toStdString(), 0);
        }
        result.push_back({match.captured(1).toInt(), entry, file.readAll()});
    }
    if (result.empty()) {
        // Only possible if the qrc object was not linked in; never run with no schema.
        throw DbError("no embedded migrations found under :/clipitto/migrations", 0);
    }
    std::sort(result.begin(), result.end(),
              [](const Migration& a, const Migration& b) { return a.version < b.version; });
    return result;
}

int schemaVersion(Db& db)
{
    Stmt stmt(db, "PRAGMA user_version");
    stmt.step();
    return int(stmt.colInt(0));
}

void migrate(Db& db)
{
    migrate(db, builtinMigrations());
}

void migrate(Db& db, const std::vector<Migration>& migrations)
{
    const int current = schemaVersion(db);
    const int latest = migrations.empty() ? 0 : migrations.back().version;
    if (current > latest) {
        throw DbError("database schema version " + std::to_string(current)
                          + " is newer than this build supports (" + std::to_string(latest) + ")",
                      0);
    }

    for (const Migration& m : migrations) {
        if (m.version <= current) {
            continue;
        }
        Transaction tx(db);
        db.exec(m.sql.constData());
        db.exec(QStringLiteral("PRAGMA user_version = %1").arg(m.version));
        tx.commit();
    }
}

} // namespace clipitto::db
