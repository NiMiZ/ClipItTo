// The sync service (src/daemon/SyncService.*): two simulated PCs through a real temporary folder, the key in a fake
// keyring, the passphrase rules, the folder going away, and that a run does not stall the GUI thread.
#include "FakeKeyStore.h"
#include "StallWatchdog.h"
#include "SyncService.h"
#include "TestDb.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace clipitto;

namespace {

struct Pc {
    TestDb t;
    std::shared_ptr<FakeKeyStore> keys = std::make_shared<FakeKeyStore>();
    SyncService svc{*t.repo, keys};
    Config config;

    explicit Pc(const QString& folder, const QString& name)
    {
        svc.setKdfCost({64, 1});
        svc.setIntervals(600000, 600000); // nothing runs by itself in a test
        config.syncEnabled = true;
        config.syncFolder = folder;
        config.syncDeviceName = name;
        config.maxHistory = 10000;
        svc.applyConfig(config, /*runSoon=*/false);
    }

    // The result of setPassphrase: (error, mismatch).
    std::pair<QString, bool> passphrase(const QByteArray& pass, bool startNew = false)
    {
        std::optional<std::pair<QString, bool>> result;
        svc.setPassphrase(config.syncFolder, pass, startNew, [&result](QString e, bool m) { result = std::make_pair(e, m); });
        if (!QTest::qWaitFor([&result] { return result.has_value(); }, 20000)) {
            qFatal("the passphrase was never answered");
        }
        return *result;
    }

    // One run, to its end.
    bool sync()
    {
        QSignalSpy done(&svc, &SyncService::finished);
        svc.syncNow();
        return !done.isEmpty() || done.wait(30000); // it can end at once (nothing to do)
    }
};

QStringList texts(TestDb& t)
{
    QStringList out;
    for (const Entry& e : t.repo->allEntries()) {
        if (!e.secret) {
            out << e.content;
        }
    }
    out.sort();
    return out;
}

} // namespace

class TestSyncService : public QObject {
    Q_OBJECT

private slots:
    void aPassphraseKeepsOnlyAKeyInTheKeyring()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        QVERIFY(!a.svc.hasPassphrase());
        const auto result = a.passphrase("a long sync passphrase");
        QVERIFY2(result.first.isEmpty(), qPrintable(result.first));
        QVERIFY(a.svc.hasPassphrase());
        QVERIFY(a.keys->stored.has_value());
        QCOMPARE(a.keys->stored->size(), 48); // salt + key, nothing of the passphrase
        QVERIFY(!a.keys->stored->contains("a long sync passphrase"));
        a.svc.removePassphrase();
        QVERIFY(!a.svc.hasPassphrase());
        QVERIFY(!a.keys->stored.has_value());
        // The config never learns it.
        QVERIFY(a.config.toToml().find("long sync") == std::string::npos);
    }

    void aSecondPcJoinsOnlyWithTheSamePassphrase()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        Pc b(dir.path(), QStringLiteral("B"));
        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        a.t.repo->ingest(QStringLiteral("from A"));
        QVERIFY(a.sync()); // writes A's file
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.clipitto-sync")}).size(), 1);

        const auto wrong = b.passphrase("another passphrase");
        QVERIFY(!wrong.first.isEmpty());
        QVERIFY(wrong.second); // the folder has files this passphrase does not open
        QVERIFY(!b.svc.hasPassphrase());
        QVERIFY(!b.keys->stored.has_value());

        QVERIFY(b.passphrase("shared passphrase").first.isEmpty());
        QCOMPARE(*b.keys->stored, *a.keys->stored); // the same group: same salt, same key
    }

    void startingANewGroupIgnoresTheOldFiles()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        Pc b(dir.path(), QStringLiteral("B"));
        QVERIFY(a.passphrase("first passphrase").first.isEmpty());
        a.t.repo->ingest(QStringLiteral("old group"));
        QVERIFY(a.sync());
        QVERIFY(b.passphrase("second passphrase", /*startNew=*/true).first.isEmpty());
        b.t.repo->ingest(QStringLiteral("new group"));
        QVERIFY(b.sync());
        QVERIFY(a.sync());
        QVERIFY2(a.svc.statusText().contains(QLatin1String("different sync passphrase")), qPrintable(a.svc.statusText()));
        QVERIFY(!texts(a.t).contains(QStringLiteral("new group")));
        QVERIFY(!texts(b.t).contains(QStringLiteral("old group")));
    }

    void twoPcsConvergeThroughTheFolder()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        Pc b(dir.path(), QStringLiteral("B"));
        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        a.t.repo->ingest(QStringLiteral("on A"));
        QVERIFY(a.sync()); // the first file is in the folder, so B can join its group
        QVERIFY(b.passphrase("shared passphrase").first.isEmpty());
        const auto pinned = a.t.repo->ingest(QStringLiteral("pinned on A"));
        a.t.repo->setPinned(pinned.id, true);
        a.t.repo->ingestSecret(QByteArray("sealed"), QStringLiteral("secret:1"), QStringLiteral("pw"), QString());
        b.t.repo->ingest(QStringLiteral("on B"));

        QSignalSpy changed(&b.svc, &SyncService::historyChanged);
        QVERIFY(a.sync()); // writes the rest
        QVERIFY(b.sync());
        QVERIFY(a.sync());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(texts(a.t), (QStringList{QStringLiteral("on A"), QStringLiteral("on B"), QStringLiteral("pinned on A")}));
        QCOMPARE(texts(a.t), texts(b.t));
        QVERIFY(b.t.repo->get(*b.t.repo->findByHash(b.t.repo->textHash(QStringLiteral("pinned on A"))))->pinned());
        QCOMPARE(b.t.repo->countSecrets(), qint64(0)); // the password copy stayed on A
        QVERIFY(a.svc.statusText().contains(QLatin1String("B")));

        // Deleting on B reaches A.
        b.svc.applyConfig(b.config, false); // makes the database track deletions
        b.t.repo->remove(*b.t.repo->findByHash(b.t.repo->textHash(QStringLiteral("on A"))));
        QVERIFY(b.sync());
        QVERIFY(a.sync());
        QVERIFY(!a.t.repo->findByHash(a.t.repo->textHash(QStringLiteral("on A"))));
    }

    // Two PCs that set the passphrase before either had put a file in the folder each made a group (salt). Entering the
    // passphrase again on both makes them find the same one.
    void twoGroupsMadeAtTheSameTimeMergeWhenThePassphraseIsEnteredAgain()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        Pc b(dir.path(), QStringLiteral("B"));
        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        QVERIFY(b.passphrase("shared passphrase").first.isEmpty());
        QVERIFY(*a.keys->stored != *b.keys->stored);
        a.t.repo->ingest(QStringLiteral("on A"));
        b.t.repo->ingest(QStringLiteral("on B"));
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        QVERIFY(a.sync());
        QVERIFY(texts(a.t) != texts(b.t));
        QVERIFY2(a.svc.statusText().contains(QLatin1String("different sync passphrase")), qPrintable(a.svc.statusText()));

        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        QVERIFY(b.passphrase("shared passphrase").first.isEmpty());
        QCOMPARE(*a.keys->stored, *b.keys->stored); // the same group now
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        QVERIFY(a.sync());
        QCOMPARE(texts(a.t), texts(b.t));
        QCOMPARE(texts(a.t).size(), 2);
    }

    void anUnchangedHistoryIsNotWrittenAgain()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        a.t.repo->ingest(QStringLiteral("one"));
        QVERIFY(a.sync());
        const QFileInfoList files = QDir(dir.path()).entryInfoList({QStringLiteral("*.clipitto-sync")});
        QCOMPARE(files.size(), 1);
        const QDateTime written = files.front().lastModified();
        QTest::qWait(1100);
        QVERIFY(a.sync());
        QVERIFY(a.sync());
        QCOMPARE(QFileInfo(files.front().absoluteFilePath()).lastModified(), written); // untouched
        a.t.repo->ingest(QStringLiteral("two"));
        QVERIFY(a.sync());
        QVERIFY(QFileInfo(files.front().absoluteFilePath()).lastModified() > written);
        // No stray temp files, whatever happened.
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1);
    }

    void aMissingFolderWaitsAndIsNeverCreated()
    {
        QTemporaryDir dir;
        const QString share = QDir(dir.path()).filePath(QStringLiteral("share"));
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("share")));
        Pc a(share, QStringLiteral("A"));
        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        a.t.repo->ingest(QStringLiteral("x"));
        QVERIFY(a.sync());
        QVERIFY(a.svc.statusText().contains(QLatin1String("Last sync")));

        // The share is unplugged.
        QDir(share).removeRecursively();
        QVERIFY(a.sync());
        QVERIFY2(a.svc.statusText().contains(QLatin1String("not available")), qPrintable(a.svc.statusText()));
        QVERIFY(a.svc.statusText().contains(QLatin1String("tries again")));
        QVERIFY(a.svc.statusLine().contains(QLatin1String("waiting")));
        QVERIFY(!QFileInfo::exists(share)); // not created behind the user's back
        QCOMPARE(a.t.repo->count(), qint64(1)); // the history is untouched

        // And plugged in again: "Sync now" ends the waiting.
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("share")));
        QVERIFY(a.sync());
        QVERIFY(!a.svc.statusText().contains(QLatin1String("Not synced")));
        QCOMPARE(QDir(share).entryList({QStringLiteral("*.clipitto-sync")}).size(), 1);
    }

    void nothingRunsWhenSyncIsOffOrHasNoKey()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        QVERIFY(a.sync()); // no passphrase yet
        QVERIFY(a.svc.statusText().contains(QLatin1String("passphrase")));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        a.config.syncEnabled = false;
        a.svc.applyConfig(a.config, false);
        QVERIFY(!a.t.repo->syncTracking());
        a.t.repo->ingest(QStringLiteral("x"));
        a.svc.syncNow();
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
        QVERIFY(a.svc.statusLine().contains(QLatin1String("off")));
    }

    void aKeyringThatCannotBeReachedIsSaid()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        a.keys->isAvailable = false;
        QString why;
        QVERIFY(!a.svc.available(&why));
        QVERIFY(!why.isEmpty());
        a.keys->isAvailable = true;
        auto b = std::make_unique<Pc>(dir.path(), QStringLiteral("B"));
        b->keys->failWrites = true;
        const auto result = b->passphrase("shared passphrase");
        QVERIFY(!result.first.isEmpty()); // no key could be kept: refused, not stored elsewhere
        QVERIFY(!b->svc.hasPassphrase());
    }

    void aRunOnALargeHistoryDoesNotStallTheGui()
    {
        QTemporaryDir dir;
        Pc a(dir.path(), QStringLiteral("A"));
        Pc b(dir.path(), QStringLiteral("B"));
        a.config.syncMaxCopies = 5000;
        a.svc.applyConfig(a.config, false);
        QVERIFY(a.passphrase("shared passphrase").first.isEmpty());
        a.t.repo->syncBatch([&] {
            for (int i = 0; i < 3000; ++i) {
                a.t.repo->ingest(QStringLiteral("a fairly ordinary copied line number %1 with some more words to it").arg(i));
            }
        });
        QVERIFY(a.sync()); // A writes
        QVERIFY(b.passphrase("shared passphrase").first.isEmpty());
        StallWatchdog wd;
        QVERIFY(b.sync()); // B reads, merges, and writes its own file
        QCOMPARE(b.t.repo->count(), qint64(3000));
        QVERIFY_NO_STALL(wd, "sync service: read, merge and write of 3000 copies");
    }
};

QTEST_MAIN(TestSyncService)
#include "tst_syncservice.moc"
