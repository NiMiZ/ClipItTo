#include "db/Migrator.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

// What encryption costs: the same file-backed workload plain and encrypted.
// Run directly: build/<preset>/tests/bench_encryption
class BenchEncryption : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    const QByteArray m_key = QByteArray(32, 'k');

    struct Timing {
        double openMs, ingestUs, searchMs, rekeyMs;
    };

    Timing run(const char* name, bool encrypted)
    {
        using namespace clipitto::db;
        const QString file = m_dir.filePath(QString::fromLatin1(name));
        const QByteArray key = encrypted ? m_key : QByteArray();
        Timing t{};
        {
            Db db(file, key);
            configureConnection(db);
            migrate(db);
            Repository repo(db);
            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 2000; ++i) {
                repo.ingest(QStringLiteral("clip %1: kubectl rollout restart deploy/api-%1 --namespace prod").arg(i));
                repo.prune(1000);
            }
            t.ingestUs = timer.nsecsElapsed() / 1000.0 / 2000;
            timer.restart();
            for (int i = 0; i < 50; ++i) {
                repo.search(QStringLiteral("rollout"), 100);
            }
            t.searchMs = timer.nsecsElapsed() / 1e6 / 50;
        }
        QElapsedTimer timer;
        timer.start();
        {
            Db db(file, key);
            Repository repo(db);
            repo.count();
        }
        t.openMs = timer.nsecsElapsed() / 1e6;
        {
            Db db(file, key);
            configureConnection(db);
            timer.restart();
            db.rekey(encrypted ? QByteArray() : m_key); // the one-time conversion
            t.rekeyMs = timer.nsecsElapsed() / 1e6;
        }
        return t;
    }

private slots:
    void compare()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
        const Timing plain = run("plain.db", false);
        const Timing enc = run("enc.db", true);
        qInfo("                 plain   encrypted");
        qInfo("open+first read  %6.2f ms  %6.2f ms", plain.openMs, enc.openMs);
        qInfo("store one copy   %6.1f us  %6.1f us", plain.ingestUs, enc.ingestUs);
        qInfo("search           %6.2f ms  %6.2f ms", plain.searchMs, enc.searchMs);
        qInfo("convert (1000)   %6.1f ms  %6.1f ms   (encrypt / decrypt)", plain.rekeyMs, enc.rekeyMs);
    }
};

QTEST_GUILESS_MAIN(BenchEncryption)
#include "bench_encryption.moc"
