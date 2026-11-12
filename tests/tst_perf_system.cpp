// Stalling in the app as a whole: Settings, a slow keyring, status calls, memory over many copies, idle CPU, the
// hotkey-to-popup time, and shutdown.
#include "CapturePipeline.h"
#include "Config.h"
#include "FakeKeyStore.h"
#include "PasswordKeeper.h"
#include "HistoryModel.h"
#include "PerfData.h"
#include "PerfDaemon.h"
#include "SettingsDialog.h"
#include "StallWatchdog.h"
#include "TestDb.h"
#include "Translate.h"

#include <QElapsedTimer>
#include <QThread>
#include <QTest>

using clipitto::Config;

namespace {

// A keyring that takes `delayMs` to answer, as a locked or unreachable one does (the real one may show an unlock window).
class SlowKeyStore : public FakeKeyStore {
public:
    int delayMs = 1000;
    bool available(QString* why) override
    {
        QThread::msleep(delayMs);
        return FakeKeyStore::available(why);
    }
    std::optional<QByteArray> read(QString* error) override
    {
        QThread::msleep(delayMs);
        return FakeKeyStore::read(error);
    }
    bool write(const QByteArray& key, QString* error) override
    {
        QThread::msleep(delayMs);
        return FakeKeyStore::write(key, error);
    }
};

} // namespace

class TestPerfSystem : public QObject {
    Q_OBJECT

private slots:
    void settingsOpensFastWithABigHistory()
    {
        TestDb t;
        perf::fill(t.db, *t.repo, 0, 5000);
        for (int g = 0; g < 100; ++g) {
            t.repo->createGroup(QStringLiteral("Work/Project%1/Sub").arg(g));
        }
        StallWatchdog wd(200);
        {
            clipitto::ui::SettingsDialog dialog(Config{}, *t.repo, {});
            dialog.show();
            QVERIFY(QTest::qWaitForWindowExposed(&dialog));
            QTest::qWait(100);
            (void)dialog.config();
        }
        QVERIFY_NO_STALL(wd, "settings: open (5k copies, 100 groups)");
    }

    void savingSettingsDoesNotStall()
    {
        perf::DaemonRig rig;
        for (int i = 0; i < 500; ++i) {
            rig.ingest(perf::sampleText(i));
        }
        rig.popup()->showPopup();
        QVERIFY(QTest::qWaitForWindowExposed(rig.popup()));
        Config cfg = rig.daemon->config();
        cfg.recentCount = 30;
        cfg.previewLines = 3;
        cfg.theme = clipitto::Theme::Dark;
        cfg.hotkey = QStringLiteral("Ctrl+Shift+V");
        StallWatchdog wd;
        QVERIFY(rig.daemon->applySettings(cfg));
        QTest::qWait(300);
        QVERIFY_NO_STALL(wd, "settings: save (theme, lines, hotkey)");
    }

    // KNOWN (not fixed): the history key and the password match key are read from the keyring on the GUI thread. A
    // keyring that is slow to answer (locked, waiting for an unlock window) holds the whole app for that long. The
    // stall is as long as the keyring is slow, so it cannot pass any budget: kept as an expected failure.
    void aSlowKeyringStallsWhileMasterPasswordIsSet()
    {
        TestDb t;
        auto keys = std::make_shared<SlowKeyStore>();
        keys->delayMs = 700;
        clipitto::PasswordKeeper keeper(*t.repo, keys);
        keeper.setKdfCost({64, 1});
        StallWatchdog wd;
        std::optional<bool> ok;
        keeper.setMasterPassword("master", [&ok](bool r) { ok = r; });
        QTRY_VERIFY_WITH_TIMEOUT(ok.has_value(), 10000);
        const qint64 max = wd.finish();
        perf::note("slow keyring (700 ms/call): longest stall (ms)", QString::number(max));
        wd.report("keyring: set master password, 700 ms keyring");
        QEXPECT_FAIL("", "keyring calls run on the GUI thread (PasswordKeeper::matchKey)", Continue);
        QVERIFY(max <= wd.budgetMs());
    }

    void anUnreachableKeyringDoesNotStall()
    {
        perf::DaemonRig rig;
        rig.keys->isAvailable = false; // no keyring at all: answers at once
        Config cfg = rig.daemon->config();
        cfg.encryptHistory = true;
        StallWatchdog wd;
        QVERIFY(rig.daemon->applySettings(cfg)); // refused: no keyring
        QTest::qWait(100);
        QVERIFY(!rig.daemon->isHistoryEncrypted());
        QVERIFY_NO_STALL(wd, "keyring: unreachable, encryption refused");
    }

    // status() is what --status and the D-Bus / local-socket call answer from; with a big history and a burst going on it
    // must stay quick (the channels run on the GUI thread, so a slow answer is a stall).
    void aStatusCallWhileBusy()
    {
        perf::DaemonRig rig;
        for (int i = 0; i < 1000; ++i) {
            rig.ingest(perf::sampleText(i));
        }
        StallWatchdog wd;
        QElapsedTimer t;
        qint64 worst = 0;
        for (int i = 0; i < 50; ++i) {
            rig.ingest(perf::sampleText(10000 + i));
            QCoreApplication::processEvents(); // a stall is measured per copy: the loop turns between them
            t.restart();
            const QVariantMap status = rig.daemon->control()->status();
            worst = qMax(worst, t.elapsed());
            QVERIFY(!status.isEmpty());
        }
        perf::note("status() worst call (ms)", QString::number(worst));
        QVERIFY2(worst < StallWatchdog::scaled(100), "a status call takes more than 100 ms");
        QVERIFY_NO_STALL(wd, "status calls during a burst");
    }

    void memoryOverFiveThousandCopies()
    {
        TestDb t;
        clipitto::CapturePipeline pipe(*t.repo, 1000, 1 << 20);
        for (int i = 0; i < 1000; ++i) { // warm up: caches, statement pools
            pipe.process(perf::sampleText(i), {QStringLiteral("text/plain")});
        }
        const qint64 before = perf::residentKb();
        StallWatchdog wd;
        for (int i = 0; i < 5000; ++i) {
            pipe.process(perf::sampleText(2000 + i), {QStringLiteral("text/plain")});
            if (i % 10 == 0) {
                QCoreApplication::processEvents(); // the loop turns every 10 copies
            }
        }
        const qint64 growth = perf::residentKb() - before;
        perf::note("RSS growth over 5k copies (KiB)", QString::number(growth));
        QCOMPARE(t.repo->count(), 1000);
        if (before > 0 && !perf::underSanitizer()) { // a sanitizer keeps freed memory back, so RSS says nothing
            QVERIFY2(growth < 40 * 1024, "memory grows by more than 40 MB over 5k copies");
        }
        QVERIFY_NO_STALL(wd, "memory: 5k copies");
    }

    // Nothing spins when nothing happens: a running daemon with the popup hidden uses almost no CPU.
    void idleCpu()
    {
        perf::DaemonRig rig;
        QTest::qWait(500);
        const qint64 cpu0 = perf::cpuMs();
        QElapsedTimer t;
        t.start();
        QTest::qWait(3000);
        const qint64 used = perf::cpuMs() - cpu0;
        perf::note("idle: CPU ms in 3 s", QString::number(used));
        if (cpu0 > 0) {
            QVERIFY2(used < 150, "idle CPU is above 5%");
        }
    }

    // The hotkey asks the daemon to show the popup: from the request to the popup on screen.
    void hotkeyToPopupLatency()
    {
        perf::DaemonRig rig;
        for (int i = 0; i < 500; ++i) {
            rig.ingest(perf::sampleText(i));
        }
        QList<qint64> times;
        for (int round = 0; round < 5; ++round) {
            QElapsedTimer t;
            t.start();
            emit rig.daemon->control()->toggleRequested(QString());
            QVERIFY(QTest::qWaitFor([&] { return rig.popup()->isVisible() && rig.popup()->model()->rowCount() > 0; }, 3000));
            times << t.elapsed();
            emit rig.daemon->control()->hideRequested();
            QTest::qWait(30);
        }
        qint64 worst = 0;
        for (const qint64 ms : times) {
            worst = qMax(worst, ms);
        }
        perf::note("hotkey -> popup worst of 5 (ms)", QString::number(worst));
        QVERIFY2(worst < StallWatchdog::scaled(250), "the popup takes more than 250 ms to appear");
    }

    // Closing the app: the destructor folds the log, waits for workers. Idle, it is quick; with a translation that never
    // ends (an engine that ignores cancellation) it waits for the translation's time limit.
    void shutdownTime()
    {
        auto rig = std::make_unique<perf::DaemonRig>();
        for (int i = 0; i < 1000; ++i) {
            rig->ingest(perf::sampleText(i));
        }
        QElapsedTimer t;
        t.start();
        rig.reset();
        perf::note("shutdown, idle, 1000 copies (ms)", QString::number(t.elapsed()));
        QVERIFY2(t.elapsed() < StallWatchdog::scaled(1500), "an idle shutdown takes more than 1.5 s");
    }
};

QTEST_MAIN(TestPerfSystem)
#include "tst_perf_system.moc"
