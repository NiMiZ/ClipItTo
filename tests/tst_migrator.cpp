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

    void pinsMigrationKeepsDataAndSavedRule()
    {
        Db db(QStringLiteral(":memory:"));
        auto migrations = builtinMigrations();
        migrate(db, {migrations[0], migrations[1]}); // schema 2
        db.exec("INSERT INTO clips(content, content_hash, is_favorite, group_name) VALUES "
                "('star', 'h1', 1, NULL), ('grouped', 'h2', 0, 'Work'), ('plain', 'h3', 0, NULL)");
        db.exec("INSERT INTO groups(path) VALUES ('Work')");
        migrate(db, {migrations[0], migrations[1], migrations[2]}); // up to schema 3
        QCOMPARE(schemaVersion(db), 3);

        Stmt s(db, "SELECT content, is_saved, pin_position FROM clips ORDER BY id");
        QStringList rows;
        while (s.step()) {
            rows << QStringLiteral("%1:%2:%3").arg(s.colText(0)).arg(s.colInt(1)).arg(s.colIsNull(2) ? "-" : "pinned");
        }
        QCOMPARE(rows, (QStringList{"star:1:-", "grouped:1:-", "plain:0:-"}));
        db.exec("UPDATE clips SET pin_position = 0 WHERE content = 'plain'");
        Stmt saved(db, "SELECT is_saved FROM clips WHERE content = 'plain'");
        QVERIFY(saved.step());
        QCOMPARE(saved.colInt(0), 1); // pinned counts as saved
        Stmt order(db, "SELECT position FROM groups WHERE path = 'Work'");
        QVERIFY(order.step());
        QVERIFY(order.colIsNull(0)); // unordered until the user moves it
    }

    void imagesMigrationAddsKindAndCascadingImageTable()
    {
        Db db(QStringLiteral(":memory:"));
        configureConnection(db); // foreign_keys = ON, as the daemon runs it
        auto migrations = builtinMigrations();
        migrate(db, {migrations[0], migrations[1], migrations[2]}); // schema 3
        db.exec("INSERT INTO clips(content, content_hash) VALUES ('old text', 'h1')");
        migrate(db);
        QCOMPARE(schemaVersion(db), 4);

        Stmt kind(db, "SELECT kind FROM clips WHERE content = 'old text'");
        QVERIFY(kind.step());
        QCOMPARE(kind.colText(0), QStringLiteral("text")); // existing rows are text

        db.exec("INSERT INTO clips(content, content_hash, kind) VALUES ('Image 2×1', 'image:x', 'image')");
        db.exec("INSERT INTO clip_images(clip_id, png, thumbnail, width, height) "
                "SELECT id, x'89504e47', x'89504e47', 2, 1 FROM clips WHERE kind = 'image'");
        db.exec("DELETE FROM clips WHERE kind = 'image'");
        Stmt left(db, "SELECT count(*) FROM clip_images");
        QVERIFY(left.step());
        QCOMPARE(left.colInt(0), 0); // the picture went with its row
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
