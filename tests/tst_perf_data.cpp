// Stalling with a lot of data: backup, startup, migration, clearing and pruning, pasting a big copy back, encrypting the
// history, and the write-ahead log. Several of these run on the GUI thread in the app (called out per test).
#include "AutoForget.h"
#include "Backup.h"
#include "Config.h"
#include "PerfData.h"
#include "PerfDaemon.h"
#include "StallWatchdog.h"
#include "TestDb.h"
#include "db/Migrator.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::Config;

namespace {

// A file database with `count` copies, closed again.
QString makeBigDb(QTemporaryDir& dir, int count, const QString& name = QStringLiteral("big.db"))
{
    const QString path = dir.filePath(name);
    clipitto::db::Db db(path);
    clipitto::db::configureConnection(db);
    clipitto::db::migrate(db);
    clipitto::db::Repository repo(db);
    perf::fill(db, repo, 0, count);
    db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
    return path;
}

} // namespace

class TestPerfData : public QObject {
    Q_OBJECT

private slots:
    // The queries behind every popup refresh (the Recent list, the Favorites row) must not read the whole history:
    // they did (an OR on is_saved blocked the index), ~190 ms each at 50k copies, on the GUI thread.
    void theRecentQueriesDoNotScanTheHistory()
    {
        QTemporaryDir dir;
        const QString path = makeBigDb(dir, 50000);
        clipitto::db::Db db(path);
        clipitto::db::configureConnection(db);
        clipitto::db::Repository repo(db);
        if (clipitto::db::encryptionSupported()) {
            db.rekey(QByteArray(32, 'k')); // encrypted pages make every page read dearer
        }
        const auto timed = [&](const char* name, const std::function<void()>& query) {
            QElapsedTimer tm;
            tm.start();
            query();
            const qint64 ms = tm.elapsed();
            perf::note(name, QString::number(ms));
            return ms;
        };
        const qint64 unpinned = timed("recentUnpinned(20) at 50k (ms)", [&] { QCOMPARE(repo.recentUnpinned(20).size(), size_t(20)); });
        const qint64 all = timed("recent(20) at 50k (ms)", [&] { QCOMPARE(repo.recent(20, false).size(), size_t(20)); });
        const qint64 unsaved = timed("recent(20, unsaved) at 50k (ms)", [&] { QCOMPARE(repo.recent(20, true).size(), size_t(20)); });
        const qint64 favorites = timed("favorites() at 50k (ms)", [&] { repo.favorites(); });
        const int limit = StallWatchdog::scaled(30);
        QVERIFY2(unpinned < limit && all < limit && unsaved < limit && favorites < limit, "a popup refresh query reads the whole history");
    }

    // Settings > Backup calls exportTo / importFrom on the GUI thread (SettingsDialog.cpp, not changed), so the time
    // measured here is how long the Settings window is frozen. Importing was quadratic (restoreFiling rewrote every
    // copy's search-index entries): 4000 copies took 12 s, so 50k took many minutes; now it is linear.
    void backupExportAndImportOf10kCopies()
    {
        TestDb source;
        const int n = qEnvironmentVariableIntValue("CLIPITTO_PERF_BACKUP_N") > 0 ? qEnvironmentVariableIntValue("CLIPITTO_PERF_BACKUP_N") : 10000;
        perf::fill(source.db, *source.repo, 0, n);
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("backup.json"));
        QString error;
        clipitto::backup::Parts parts;
        parts.copies = true;
        QElapsedTimer t;
        t.start();
        QVERIFY2(clipitto::backup::exportTo(file, *source.repo, Config{}, nullptr, parts, {}, &error), qPrintable(error));
        const qint64 exportMs = t.restart();
        perf::note("backup export (ms)", QString::number(exportMs));
        TestDb target;
        const auto report = clipitto::backup::importFrom(file, parts, *target.repo, nullptr, {}, &error);
        const qint64 importMs = t.elapsed();
        QVERIFY2(report.has_value(), qPrintable(error));
        QCOMPARE(report->added, n);
        perf::note("backup export (ms) / file bytes", QStringLiteral("%1 / %2").arg(exportMs).arg(QFileInfo(file).size()));
        perf::note("backup import (ms)", QString::number(importMs));
        QVERIFY2(exportMs < StallWatchdog::scaled(15000), "exporting 50k copies takes more than 15 s");
        QVERIFY2(importMs < StallWatchdog::scaled(30000), "importing 50k copies takes more than 30 s");
    }

    void importingBackupIsLinear()
    {
        TestDb source;
        perf::fill(source.db, *source.repo, 0, 3000);
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("backup.json"));
        clipitto::backup::Parts parts;
        parts.copies = true;
        QString error;
        QVERIFY(clipitto::backup::exportTo(file, *source.repo, Config{}, nullptr, parts, {}, &error));
        TestDb target;
        QElapsedTimer t;
        t.start();
        QVERIFY(clipitto::backup::importFrom(file, parts, *target.repo, nullptr, {}, &error).has_value());
        perf::note("import 3000 copies (ms)", QString::number(t.elapsed()));
        QVERIFY2(t.elapsed() < StallWatchdog::scaled(2500), "importing 3000 copies takes more than 2.5 s (was 9 s)");
    }

    // Startup runs on the GUI thread before the popup exists: open, migrate, build the popup's first list.
    void startupWithA50kDatabase()
    {
        perf::DaemonRig rig("max_history = 100000\n", /*start=*/false);
        QDir().mkpath(rig.dir.filePath(QStringLiteral("data")));
        const QString path = makeBigDb(rig.dir, 50000, QStringLiteral("data/history.db"));
        QVERIFY(QFileInfo::exists(path));
        QElapsedTimer t;
        t.start();
        StallWatchdog wd;
        QVERIFY(rig.daemon->start() == clipitto::Daemon::StartResult::Ok);
        const qint64 ms = t.elapsed();
        perf::note("daemon start with a 50k database (ms)", QString::number(ms));
        QVERIFY2(ms < StallWatchdog::scaled(2000), "starting with 50k copies takes more than 2 s");
        wd.finish();
        rig.popup()->showPopup();
        QTest::qWait(100);
        QCOMPARE(rig.repo().count(), 50000);
    }

    // The heaviest migration step (003 rebuilds the clips table) on 50k rows.
    void migrationOfA50kDatabase()
    {
        QTemporaryDir dir;
        clipitto::db::Db db(dir.filePath(QStringLiteral("old.db")));
        clipitto::db::configureConnection(db);
        std::vector<clipitto::db::Migration> old;
        for (auto& m : clipitto::db::builtinMigrations()) {
            if (m.version <= 2) {
                old.push_back(m);
            }
        }
        clipitto::db::migrate(db, old);
        {
            clipitto::db::Transaction tx(db);
            clipitto::db::Stmt ins(db, "INSERT INTO clips(content, content_hash) VALUES (?1, ?2)");
            for (int i = 0; i < 20000; ++i) {
                ins.reset();
                ins.bindText(1, perf::sampleText(i)).bindText(2, QStringLiteral("h%1").arg(i)).run();
            }
            tx.commit();
        }
        QElapsedTimer t;
        t.start();
        clipitto::db::migrate(db);
        const qint64 ms = t.elapsed();
        perf::note("migrate 50k rows from schema 2 (ms)", QString::number(ms));
        QVERIFY2(ms < StallWatchdog::scaled(8000), "migrating 50k rows takes more than 8 s");
        clipitto::db::Repository repo(db);
        QCOMPARE(repo.count(), 20000);
    }

    // KNOWN (not changed): deleting copies costs about a millisecond each in a debug build, mostly the search index
    // (the trigram entries of every deleted text are written as deletions). A history over its limit (a lowered
    // max_history, a big restored backup) is pruned in one statement on the GUI thread, also at startup: with 50k copies
    // over the limit that is a minute in debug builds. Measured on 5000 copies, held to a sanity limit.
    void clearAndPrune()
    {
        TestDb t;
        perf::fill(t.db, *t.repo, 0, 3000);
        QElapsedTimer tm;
        tm.start();
        const qint64 pruned = t.repo->prune(500); // as after lowering max_history
        const qint64 pruneMs = tm.restart();
        t.repo->clearHistory(true);
        const qint64 clearMs = tm.elapsed();
        perf::note("prune 2500 rows (ms) / clear 1000 (ms)", QStringLiteral("%1 / %2").arg(pruneMs).arg(clearMs));
        QCOMPARE(pruned, 2500);
        QCOMPARE(t.repo->count(), 0);
        QVERIFY2(pruneMs < StallWatchdog::scaled(20000), "pruning 2500 copies takes more than 20 s");
        // And the everyday case: one new copy over the limit prunes one old one.
        perf::fill(t.db, *t.repo, 0, 5000);
        qint64 worst = 0;
        for (int i = 0; i < 100; ++i) {
            tm.restart();
            t.repo->ingest(perf::sampleText(70000 + i));
            t.repo->prune(5000);
            worst = qMax(worst, tm.elapsed());
        }
        perf::note("prune: worst single copy over the limit at 5k (ms)", QString::number(worst));
        QVERIFY2(worst < StallWatchdog::scaled(100), "one copy over the limit takes more than 100 ms to store");
    }

    // Auto-forget on the GUI thread (Daemon::runAutoForget: at start, once a minute, when the popup opens). A new rule
    // on a long history removes its backlog in small steps, 100 copies a run, so the window never freezes; and a run that
    // finds nothing new to remove is a single indexed query.
    void autoForgetBacklogGoesInSmallSteps()
    {
        perf::DaemonRig rig("auto_forget = [\"age 30d\"]\n", /*start=*/false);
        QDir().mkpath(rig.dir.filePath(QStringLiteral("data")));
        const int n = 3000;
        {
            clipitto::db::Db db(rig.dir.filePath(QStringLiteral("data/history.db")));
            clipitto::db::configureConnection(db);
            clipitto::db::migrate(db);
            QDateTime clock = QDateTime::currentDateTimeUtc().addDays(-100);
            clipitto::db::Repository repo(db, [&clock] { return clock = clock.addMSecs(1); });
            perf::fill(db, repo, 0, n);
            db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        }
        QVERIFY(rig.daemon->start() == clipitto::Daemon::StartResult::Ok); // the first 100 go in here
        QVERIFY(rig.repo().count() > 0);
        StallWatchdog wd(500);
        QTRY_COMPARE_WITH_TIMEOUT(rig.repo().count(), 0, StallWatchdog::scaled(60000));
        QVERIFY_NO_STALL(wd, "auto-forget: a 3000-copy backlog");
    }

    void autoForgetPatternScanOn50kCopies()
    {
        TestDb t;
        perf::fill(t.db, *t.repo, 0, 50000);
        clipitto::autoforget::Pruner pruner(*t.repo, [] { return QDateTime::currentDateTimeUtc().addDays(1); });
        clipitto::AutoForgetRule rule;
        rule.type = clipitto::AutoForgetRule::Type::Secret;
        rule.amount = 1;
        pruner.setRules({rule});
        QVERIFY_FAST(500, "auto-forget: one scan step at 50k", pruner.run());
        QVERIFY(pruner.hasMore()); // 500 of 50000 read: the rest in later steps
        // Once everything was read, a run only lists the candidates: one indexed query.
        int runs = 1;
        while (pruner.hasMore() && runs < 1000) {
            pruner.run();
            ++runs;
        }
        QVERIFY(!pruner.hasMore());
        QVERIFY_FAST(500, "auto-forget: a run with nothing new at 50k", pruner.run());
        QCOMPARE(t.repo->count(), 50000);
    }

    // Pasting a 1 MB copy: the popup tells the daemon, which puts it on the clipboard.
    void pastingALargeCopyBack()
    {
        perf::DaemonRig rig("max_item_bytes = 4194304\n");
        QString text;
        for (int i = 0; text.size() < 1024 * 1024; ++i) {
            text += perf::sampleText(i) + QLatin1Char('\n');
        }
        rig.popup()->showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(rig.popup()));
        StallWatchdog wd;
        emit rig.popup()->textActivated(text);
        QTest::qWait(150);
        QCOMPARE(QGuiApplication::clipboard()->text().size(), text.size());
        QVERIFY_NO_STALL(wd, "paste back a 1 MB copy");
    }

    // Turning encryption on for a 50k history: the rewrite runs on a worker; the GUI thread stays free.
    void encryptingA50kDatabase()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("this build has no encryption");
        }
        perf::DaemonRig rig("max_history = 100000\n", /*start=*/false);
        QDir().mkpath(rig.dir.filePath(QStringLiteral("data")));
        makeBigDb(rig.dir, 20000, QStringLiteral("data/history.db"));
        QVERIFY(rig.daemon->start() == clipitto::Daemon::StartResult::Ok);
        QSignalSpy finished(rig.daemon.get(), &clipitto::Daemon::encryptionFinished);
        Config cfg = rig.daemon->config();
        cfg.encryptHistory = true;
        StallWatchdog wd;
        QElapsedTimer t;
        t.start();
        QVERIFY(rig.daemon->applySettings(cfg));
        perf::note("encrypt: applySettings call (ms)", QString::number(t.elapsed()));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 60000);
        QVERIFY2(finished.at(0).at(1).toString().isEmpty(), qPrintable(finished.at(0).at(1).toString()));
        perf::note("encrypt a 50k database (ms)", QString::number(t.elapsed()));
        perf::note("encrypt: longest stall ended at (ms) / finished at (ms)", QStringLiteral("%1 / %2").arg(wd.maxStallEndedAtMs()).arg(t.elapsed()));
        QVERIFY(rig.daemon->isHistoryEncrypted());
        QVERIFY_NO_STALL(wd, "encrypt a 50k database");
    }

    // The write-ahead log is folded back: after thousands of single-copy writes it is small, and after Clear it is empty.
    void theWalDoesNotGrowWithoutBound()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("wal.db"));
        clipitto::db::Db db(path);
        clipitto::db::configureConnection(db);
        clipitto::db::migrate(db);
        clipitto::db::Repository repo(db);
        qint64 biggest = 0;
        for (int i = 0; i < 4000; ++i) { // each ingest is its own transaction, as live copies are
            repo.ingest(perf::sampleText(i));
            if (i % 500 == 0) {
                biggest = qMax(biggest, QFileInfo(path + QStringLiteral("-wal")).size());
            }
        }
        perf::note("largest WAL during 4k single writes (bytes)", QString::number(biggest));
        QVERIFY2(biggest < 16 * 1024 * 1024, "the write-ahead log grew past 16 MB");
        repo.clearHistory(true);
        QCOMPARE(QFileInfo(path + QStringLiteral("-wal")).size(), qint64(0));
    }
};

QTEST_MAIN(TestPerfData)
#include "tst_perf_data.moc"
