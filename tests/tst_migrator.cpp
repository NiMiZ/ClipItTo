#include "db/Migrator.h"
#include "db/Sqlite.h"

#include <QTest>

using namespace clipitto::db;

namespace {

bool tableExists(Db& db, const char* name)
{
    Stmt s(db, "SELECT 1 FROM sqlite_master WHERE name = ?1");
    s.bindText(1, QString::fromLatin1(name));
    return s.step();
}

} // namespace

class TestMigrator : public QObject {
    Q_OBJECT

private slots:
    void builtinMigrationsAreEmbedded()
    {
        const auto migrations = builtinMigrations();
        QVERIFY(!migrations.empty());
        QCOMPARE(migrations.front().version, 1);
        QCOMPARE(migrations.front().name, QStringLiteral("001_init.sql"));
        for (std::size_t i = 1; i < migrations.size(); ++i) {
            QVERIFY(migrations[i].version > migrations[i - 1].version);
        }
    }

    void freshDatabaseReachesLatestVersion()
    {
        Db db(QStringLiteral(":memory:"));
        QCOMPARE(schemaVersion(db), 0);
        migrate(db);
        QCOMPARE(schemaVersion(db), builtinMigrations().back().version);
        QVERIFY(tableExists(db, "clips"));
        QVERIFY(tableExists(db, "clips_fts"));
        QVERIFY(tableExists(db, "idx_clips_saved_lastused"));
    }

    void migrateIsIdempotent()
    {
        Db db(QStringLiteral(":memory:"));
        migrate(db);
        migrate(db);
        QCOMPARE(schemaVersion(db), builtinMigrations().back().version);
    }

    void appliesOnlyNewerMigrationsInOrder()
    {
        Db db(QStringLiteral(":memory:"));
        const std::vector<Migration> v1 = {{1, QStringLiteral("001"), "CREATE TABLE a(x);"}};
        migrate(db, v1);
        QCOMPARE(schemaVersion(db), 1);

        auto v3 = v1;
        v3.push_back({2, QStringLiteral("002"), "CREATE TABLE b(x);"});
        v3.push_back({3, QStringLiteral("003"), "INSERT INTO b VALUES (1);"});
        migrate(db, v3); // would fail on "table a already exists" if 001 ran again
        QCOMPARE(schemaVersion(db), 3);
        QVERIFY(tableExists(db, "b"));
    }

    void failedMigrationRollsBack()
    {
        Db db(QStringLiteral(":memory:"));
        const std::vector<Migration> migrations = {
            {1, QStringLiteral("001"), "CREATE TABLE a(x);"},
            {2, QStringLiteral("002"), "CREATE TABLE b(x); THIS IS NOT SQL;"},
        };
        QVERIFY_THROWS_EXCEPTION(DbError, migrate(db, migrations));
        QCOMPARE(schemaVersion(db), 1);
        QVERIFY(tableExists(db, "a"));
        QVERIFY(!tableExists(db, "b"));
    }

    void groupsMigrationAdoptsExistingGroupsWithParents()
    {
        Db db(QStringLiteral(":memory:"));
        auto migrations = builtinMigrations();
        migrate(db, {migrations.front()}); // schema 1 only
        db.exec("INSERT INTO clips(content, content_hash, group_name) VALUES "
                "('a', 'h1', 'Work/Servers/Prod'), ('b', 'h2', 'Snippets'), ('c', 'h3', NULL)");
        migrate(db);

        Stmt s(db, "SELECT path FROM groups ORDER BY path");
        QStringList paths;
        while (s.step()) {
            paths << s.colText(0);
        }
        QCOMPARE(paths, (QStringList{QStringLiteral("Snippets"), QStringLiteral("Work"), QStringLiteral("Work/Servers"),
                                     QStringLiteral("Work/Servers/Prod")}));
    }

    void newerSchemaIsRejected()
    {
        Db db(QStringLiteral(":memory:"));
        db.exec("PRAGMA user_version = 999");
        QVERIFY_THROWS_EXCEPTION(DbError, migrate(db));
    }
};

QTEST_GUILESS_MAIN(TestMigrator)
#include "tst_migrator.moc"
