// The stall watchdog itself: it must see a blocked event loop and stay quiet on a free one.
#include "StallWatchdog.h"

#include <QTest>
#include <QThread>

class TestPerfWatchdog : public QObject {
    Q_OBJECT

private slots:
    void aFreeLoopShowsNoStall()
    {
        StallWatchdog wd;
        QTest::qWait(200);
        QVERIFY2(wd.finish() < 50, "an idle loop must not look stalled");
        QVERIFY(wd.report("watchdog: idle loop"));
    }

    void aBlockedLoopIsMeasured()
    {
        StallWatchdog wd(100);
        QTest::qWait(30);
        QTimer::singleShot(0, [] { QThread::msleep(300); }); // a slot that blocks
        QTest::qWait(500);
        const qint64 max = wd.finish();
        QVERIFY2(max >= 250 && max < 450, qPrintable(QString::number(max)));
    }

    void aSynchronousStallCountsEvenWithoutSpinningTheLoopAgain()
    {
        StallWatchdog wd;
        QTest::qWait(30);
        QThread::msleep(200); // blocks the test body itself
        const qint64 max = wd.finish();
        QVERIFY2(max >= 150, qPrintable(QString::number(max)));
    }

    void restartForgetsEarlierStalls()
    {
        StallWatchdog wd;
        QTest::qWait(20);
        QThread::msleep(150);
        QTest::qWait(20);
        QVERIFY(wd.maxStallMs() >= 100);
        wd.restart();
        QTest::qWait(100);
        QVERIFY(wd.finish() < 50);
    }

    void theBudgetFollowsTheEnvironment()
    {
        qunsetenv("CI");
        qunsetenv("GITHUB_ACTIONS");
        qunsetenv("CLIPITTO_PERF_STRICT");
        QCOMPARE(StallWatchdog::scaled(100), 100);
        qputenv("CLIPITTO_PERF_STRICT", "1");
        QCOMPARE(StallWatchdog::scaled(100), 50);
        qunsetenv("CLIPITTO_PERF_STRICT");
        qputenv("CI", "true");
        QCOMPARE(StallWatchdog::scaled(100), 500);
        qunsetenv("CI");
    }
};

QTEST_MAIN(TestPerfWatchdog)
#include "tst_perf_watchdog.moc"
