#include "ControlChannel.h"
#include "Daemon.h"
#include "FakeKeyStore.h"
#include "SelfTest.h"
#include "SelfTestWindow.h"
#include "SettingsDialog.h"
#include "db/Repository.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace clipitto;
using clipitto::selftest::Outcome;
using clipitto::selftest::Report;

namespace {

class FakeControl : public ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

const QString kSecret = QStringLiteral("SECRET-COPY-5d1c");

} // namespace

// The daemon's side of "Check this installation" (roadmap 31) and the Settings window: the live capture check, the paste check,
// copying the report, and what a report may contain. Offscreen; no real keys are typed and no real keyring is asked.
class TestSelfTestDaemon : public QObject {
    Q_OBJECT

private:
    DaemonOptions options() const
    {
        DaemonOptions o;
        o.configPath = m_dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = m_dir.filePath(QStringLiteral("data/history.db"));
        o.debugLogPath = m_dir.filePath(QStringLiteral("debug.log"));
        o.syntheticPaste = [](PasteKeys) { return false; };
        return o;
    }

    Report runToEnd(Daemon& daemon, const selftest::PasteBox* box = nullptr)
    {
        selftest::Runner* runner = daemon.createSelfTest(box);
        QSignalSpy done(runner, &selftest::Runner::finished);
        runner->start();
        if (!done.wait(120000) && done.isEmpty()) {
            return {};
        }
        return qvariant_cast<Report>(done.first().at(0));
    }

    bool historyFileHolds(const QByteArray& needle) const
    {
        for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            QFile file(options().databasePath + suffix);
            if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray bytes = file.readAll();
            file.close();
            if (bytes.contains(needle)) {
                return true;
            }
        }
        return false;
    }

    static bool hasTestCopy(Daemon& daemon)
    {
        for (const Entry& entry : daemon.repository()->allEntries()) {
            if (entry.content.contains(QLatin1String("self-test copy")) || entry.content.contains(QLatin1String("check this installation"))) {
                return true;
            }
        }
        return false;
    }

    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Report>();
        qRegisterMetaType<selftest::Result>();
        QVERIFY(m_dir.isValid());
    }

    void captureCheckSeesTheTestCopyAndRemovesItSecurely()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        emit daemon.control()->ingestRequested(QStringLiteral("a normal copy"), {QStringLiteral("text/plain")});
        const qint64 before = daemon.repository()->count();
        // Remember the test text when it appears on the clipboard, to look for it in the file afterwards.
        QString marker;
        connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [&marker] {
            const QString text = QApplication::clipboard()->text();
            if (text.startsWith(QLatin1String("ClipItTo self-test copy"))) {
                marker = text;
            }
        });
        const Report report = runToEnd(daemon);
        QVERIFY2(!report.results.isEmpty(), "the self-test did not finish");
        const selftest::Result* capture = report.find(QStringLiteral("capture_copy"));
        QVERIFY(capture);
        QVERIFY2(capture->outcome == Outcome::Pass, qPrintable(capture->detail));
        QCOMPARE(daemon.repository()->count(), before);
        QVERIFY(!hasTestCopy(daemon));
        QVERIFY(!marker.isEmpty());
        QVERIFY2(!historyFileHolds(marker.toUtf8()), "the test copy is still in the history file");
        QVERIFY2(!report.text().contains(marker), "the marker must not be in the report");
        // The user's own copy is untouched.
        QVERIFY(historyFileHolds("a normal copy"));
        disconnect(QApplication::clipboard(), nullptr, this, nullptr);
    }

    void aReportNeverCarriesWhatWasCopiedOrTheKeys()
    {
        DaemonOptions o = options();
        auto keys = std::make_shared<FakeKeyStore>();
        keys->stored = kSecret.toUtf8();
        o.keyStore = o.matchKeyStore = o.syncKeyStore = keys;
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        emit daemon.control()->ingestRequested(kSecret + QStringLiteral(" copied text"), {QStringLiteral("text/plain")});
        QApplication::clipboard()->setText(kSecret + QStringLiteral(" on the clipboard"));
        const Report report = runToEnd(daemon);
        QVERIFY(report.results.size() >= 18);
        QVERIFY2(!report.text().contains(kSecret), qPrintable(report.text()));
        // The same text goes over the control channel.
        QVERIFY(!report.toMap().value(QStringLiteral("report")).toString().contains(kSecret));
        // The paths of ClipItTo's own files are in it, as intended.
        QVERIFY(report.text().contains(options().databasePath));
        QVERIFY(report.text().contains(options().configPath));
        QVERIFY(report.find(QStringLiteral("history_db"))->outcome == Outcome::Pass);
        QVERIFY(report.find(QStringLiteral("debug_log"))->detail.contains(options().debugLogPath));
    }

    void theControlChannelAnswersWithTheReport()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        std::optional<QVariantMap> reply;
        daemon.control()->runSelfTest([&reply](const QVariantMap& map) { reply = map; });
        QVERIFY(QTest::qWaitFor([&reply] { return reply.has_value(); }, 120000));
        QVERIFY(reply->value(QStringLiteral("report")).toString().contains(QLatin1String("Summary:")));
        QVERIFY(reply->contains(QStringLiteral("fail")));
    }

    void twoCallersShareOneRun()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        selftest::Runner* first = daemon.createSelfTest();
        QSignalSpy done(first, &selftest::Runner::finished);
        first->start();
        QCOMPARE(daemon.createSelfTest(), first); // asked again while running: the same run
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 120000);
    }

    void settingsButtonOpensTheWindowAndCopyReportIsNotCaptured()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        ui::SettingsDialog dialog(daemon.config(), *daemon.repository(), {});
        QVERIFY(!dialog.selfTestButton()->isEnabled()); // nothing to run until the daemon gives the checks
        dialog.setSelfTest(daemon.selfTestSupport());
        QVERIFY(dialog.selfTestButton()->isEnabled());
        QVERIFY(!dialog.selfTestWindow());
        dialog.selfTestButton()->click();
        ui::SelfTestWindow* window = dialog.selfTestWindow();
        QVERIFY(window);
        QVERIFY(window->isVisible());
        QSignalSpy finished(window, &ui::SelfTestWindow::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 120000);
        QVERIFY(!window->isRunning());
        QCOMPARE(window->resultList()->count(), int(window->report().results.size()));
        QVERIFY(window->resultList()->count() >= 18);
        QVERIFY(window->resultList()->item(0)->text().contains(QLatin1String("Program and session")));
        QVERIFY(window->resultList()->item(1)->text().contains(QRegularExpression(QStringLiteral("PASS|WARN|FAIL|SKIPPED"))));
        QVERIFY(window->privacyLabel()->text().contains(QLatin1String("never contains what you copied")));
        QVERIFY(window->copyButton()->isEnabled());
        // Wait for the test text's echo to be over, then copy the report.
        QTest::qWait(300);
        const qint64 before = daemon.repository()->count();
        window->copyButton()->click();
        QCOMPARE(QApplication::clipboard()->text(), window->reportText());
        QVERIFY(QApplication::clipboard()->text().contains(QLatin1String("Summary:")));
        QTest::qWait(600); // long enough for a capture to have stored it, had it been seen as a copy
        QCOMPARE(daemon.repository()->count(), before);
        QVERIFY(!hasTestCopy(daemon));
        window->close();
    }

    void pasteCheckTypesIntoTheWindowsTestBox()
    {
        DaemonOptions o = options();
        QPlainTextEdit* box = nullptr;
        int typed = 0;
        // A stand-in for the keys arriving: the focused box receives what the clipboard holds.
        o.syntheticPaste = [&box, &typed](PasteKeys) {
            ++typed;
            if (box) {
                box->insertPlainText(QApplication::clipboard()->text());
            }
            return true;
        };
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        ui::SettingsDialog dialog(daemon.config(), *daemon.repository(), {});
        dialog.setSelfTest(daemon.selfTestSupport());
        dialog.selfTestButton()->click();
        ui::SelfTestWindow* window = dialog.selfTestWindow();
        QVERIFY(window);
        box = window->pasteBox();
        QSignalSpy finished(window, &ui::SelfTestWindow::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 120000);
        QCOMPARE(typed, 1);
        const selftest::Result* paste = window->report().find(QStringLiteral("paste"));
        QVERIFY(paste);
        QVERIFY2(paste->outcome == Outcome::Pass, qPrintable(paste->detail));
        // The marker the paste check used is a fixed-form text, not a copy of anything of the user's, and it is not in the history.
        QVERIFY(!hasTestCopy(daemon));
        window->close();
    }

    void pasteCheckSaysWhenKeysCannotBeTypedOrDoNotArrive()
    {
        DaemonOptions o = options();
        bool canType = false;
        o.syntheticPaste = [&canType](PasteKeys) { return canType; };
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        QString text;
        selftest::PasteBox box{[] {}, [&text] { return text; }, nullptr};
        Report report = runToEnd(daemon, &box);
        QCOMPARE(report.find(QStringLiteral("paste"))->outcome, Outcome::Warn); // this session cannot type
        canType = true; // typed, but the box stays empty
        report = runToEnd(daemon, &box);
        QCOMPARE(report.find(QStringLiteral("paste"))->outcome, Outcome::Fail);
        // Without a box (the CLI) it is skipped.
        report = runToEnd(daemon);
        QCOMPARE(report.find(QStringLiteral("paste"))->outcome, Outcome::Skipped);
    }

    void keyringsAreReadForRealOnlyWhereTheFeatureIsOn()
    {
        DaemonOptions o = options();
        auto history = std::make_shared<FakeKeyStore>();
        auto match = std::make_shared<FakeKeyStore>();
        match->isAvailable = false;
        o.keyStore = history;
        o.matchKeyStore = match;
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        const Report report = runToEnd(daemon);
        QCOMPARE(report.find(QStringLiteral("keyring_history"))->outcome, Outcome::Pass); // reachable, not needed
        QCOMPARE(report.find(QStringLiteral("keyring_passwords"))->outcome, Outcome::Skipped); // down, not needed
        QCOMPARE(report.find(QStringLiteral("keyring_sync"))->outcome, Outcome::Skipped); // no store
    }
};

QTEST_MAIN(TestSelfTestDaemon)
#include "tst_selftest_daemon.moc"
