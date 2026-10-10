#pragma once

#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QDateTime>

#include <memory>

// In-memory, migrated database plus a repository whose clock advances 1 ms per reading,
// so ordering by last_used is deterministic even for rapid inserts.
struct TestDb {
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), Qt::UTC);
    clipitto::db::Db db{QStringLiteral(":memory:")};
    std::unique_ptr<clipitto::db::Repository> repo;

    TestDb()
    {
        clipitto::db::configureConnection(db);
        clipitto::db::migrate(db);
        repo = std::make_unique<clipitto::db::Repository>(db, [this] {
            clock = clock.addMSecs(1);
            return clock;
        });
    }
};
