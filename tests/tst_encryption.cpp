#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace clipitto::db;

// Encrypted history files (SQLite3 Multiple Ciphers), at the database layer.
class TestEncryption : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    const QByteArray m_key = QByteArray::fromHex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");

    QString path(const char* name) const { return m_dir.filePath(QString::fromLatin1(name)); }

    static bool fileContains(const QString& file, const QByteArray& needle)
    {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) && f.readAll().contains(needle);
    }

    void fill(Db& db)
    {
        configureConnection(db);
        migrate(db);
        Repository repo(db);
        const auto id = repo.ingest(QStringLiteral("top secret clipboard text")).id;
        repo.setNote(id, QStringLiteral("launch codes"));
        repo.setGroup(id, QStringLiteral("Work/Secrets"));
    }

private slots:
    void initTestCase()
    {
        if (!encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
    }

    void probeTellsTheStates()
    {
        QCOMPARE(probeFile(path("nope.db")), FileState::Missing);
        {
            Db plain(path("plain.db"));
            fill(plain);
        }
        QCOMPARE(probeFile(path("plain.db")), FileState::Plain);
        {
            Db enc(path("enc.db"), m_key);
            fill(enc);
        }
        QCOMPARE(probeFile(path("enc.db")), FileState::Encrypted);
    }

    void damagedPlainFileIsNotMistakenForEncrypted()
    {
        QFile f(path("broken.db"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray("SQLite format 3\0", 16) + QByteArray(4096, 'x'));
        f.close();
        QVERIFY_THROWS_EXCEPTION(DbError, probeFile(path("broken.db")));
    }

    void encryptedFileNeedsTheRightKey()
    {
        {
            Db enc(path("keyed.db"), m_key);
            fill(enc);
        }
        QVERIFY(!fileContains(path("keyed.db"), "top secret"));
        QVERIFY(!fileContains(path("keyed.db"), "launch codes"));
        QByteArray wrong = m_key;
        wrong[0] = char(0xff);
        QVERIFY_THROWS_EXCEPTION(DbError, Db(path("keyed.db"), wrong));
        Db right(path("keyed.db"), m_key);
        Repository repo(right);
        QCOMPARE(repo.search(QStringLiteral("secret clip")).size(), 1u); // FTS works encrypted
        QCOMPARE(repo.groups(), (QStringList{QStringLiteral("Work"), QStringLiteral("Work/Secrets")}));
    }

    void rekeyConvertsInPlaceBothWays()
    {
        const QString file = path("live.db");
        Db db(file);
        fill(db);
        Repository repo(db); // statements prepared before the conversion stay usable
        QVERIFY(fileContains(file, "top secret") || QFile::exists(file + QStringLiteral("-wal")));

        db.rekey(m_key);
        QCOMPARE(probeFile(file), FileState::Encrypted);
        QVERIFY(!fileContains(file, "top secret"));
        QCOMPARE(repo.search(QStringLiteral("launch")).size(), 1u);
        repo.ingest(QStringLiteral("added after encrypting"));

        db.rekey({});
        QCOMPARE(probeFile(file), FileState::Plain);
        QCOMPARE(repo.count(), 2);
        QCOMPARE(repo.search(QStringLiteral("after encrypting")).size(), 1u);
    }
};

QTEST_GUILESS_MAIN(TestEncryption)
#include "tst_encryption.moc"
