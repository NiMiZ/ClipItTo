#include "Backup.h"
#include "ClipFilter.h"
#include "ControlChannel.h"
#include "Daemon.h"
#include "DesktopIntegration.h"
#include "FakeKeyStore.h"
#include "db/Migrator.h"
#include "db/Sqlite.h"
#include "GroupPath.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PasswordKeeper.h"
#include "PopupWindow.h"
#include "SecretScan.h"
#include "SearchBar.h"
#include "SyncService.h"
#include "SyntheticPaste.h"
#include "db/Repository.h"

#include <QBuffer>
#include <QMenu>
#include <QClipboard>
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QFile>
#include <QLoggingCategory>
#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <set>

using clipitto::Config;
using clipitto::ControlChannel;
using clipitto::DesktopIntegration;
using clipitto::PasteKeys;
using clipitto::PasteTarget;
using clipitto::SessionEnv;
using clipitto::Daemon;
using clipitto::DaemonOptions;
using clipitto::Entry;
using clipitto::ui::HistoryModel;

namespace {

// Stands in for the platform channel: tests emit its request signals directly.
class FakeControl : public ControlChannel {
public:
    explicit FakeControl(StartResult result = StartResult::Ok) : m_result(result) {}
    StartResult start(QString* error) override
    {
        if (error && m_result != StartResult::Ok) {
            *error = QStringLiteral("fake refusal");
        }
        return m_result;
    }

private:
    StartResult m_result;
};

// Records what the daemon asks of the desktop shell.
class FakeIntegration : public DesktopIntegration {
public:
    explicit FakeIntegration(bool available) : m_available(available) {}

    QString name() const override { return QStringLiteral("fake"); }
    bool isAvailable() const override { return m_available; }
    void activatePopup() override { ++activations; }
    void setClipboard(const QString& text) override { clipboardTexts << text; }
    void setClipboardImage(const QByteArray& png) override { clipboardImages << png; }
    void paste(PasteKeys keys) override { pastes << keys; }
    void rememberPasteTarget() override { ++targetAsks; }
    PasteTarget pasteTarget() const override { return target; }

    void setAvailable(bool available)
    {
        m_available = available;
        emit availabilityChanged(available);
    }

    int activations = 0;
    QStringList clipboardTexts;
    QList<QByteArray> clipboardImages;
    QList<PasteKeys> pastes;
    int targetAsks = 0;
    PasteTarget target = PasteTarget::unsupported();

private:
    bool m_available;
};

} // namespace

// Platform-independent daemon behavior (runs on Linux and Windows, offscreen).
class TestDaemon : public QObject {
    Q_OBJECT

private:
    DaemonOptions options() const
    {
        DaemonOptions o;
        o.configPath = m_dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = m_dir.filePath(QStringLiteral("data/history.db"));
        o.syntheticPaste = [](PasteKeys) { return false; }; // never type real keys on the test machine
        return o;
    }

    void writeConfig(const QByteArray& text) const
    {
        QFile file(options().configPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(text);
    }

    QTemporaryDir m_dir;

    // --- Passwords and grouping, end to end (the daemon, its popup, a real history file) ---

    enum class VaultMode { Master, Open, None };

    // A master password ("master"), the open mode, or neither. Cheap Argon2 costs.
    static bool setUpVault(Daemon& d, VaultMode mode)
    {
        if (mode == VaultMode::None) {
            return true;
        }
        d.passwords()->setKdfCost({64, 1});
        std::optional<bool> ok;
        const auto done = [&ok](bool result) { ok = result; };
        if (mode == VaultMode::Master) {
            d.passwords()->setMasterPassword("master", done);
        } else {
            d.passwords()->useWithoutMasterPassword(done);
        }
        return QTest::qWaitFor([&ok] { return ok.has_value(); }, 10000) && *ok;
    }

    static bool unlockWith(Daemon& d, const QByteArray& password)
    {
        std::optional<bool> ok;
        d.passwords()->unlock(password, [&ok](bool result) { ok = result; });
        return QTest::qWaitFor([&ok] { return ok.has_value(); }, 10000) && *ok;
    }

    QString lockLine(Daemon& d) { return d.status().value(QStringLiteral("lock_on_screen_lock")).toString(); }

    // A daemon with a master password, a password copy and a fake watcher; unlocked when `unlock`.
    struct ScreenLockEnv {
        std::shared_ptr<clipitto::FakeScreenLockWatcher> watcher = std::make_shared<clipitto::FakeScreenLockWatcher>();
        std::unique_ptr<Daemon> d;
        Entry secret;
    };

    void startScreenLockEnv(ScreenLockEnv& e, VaultMode mode, bool unlock = true)
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        o.screenLockWatcher = e.watcher;
        e.d = std::make_unique<Daemon>(o, std::make_unique<FakeControl>());
        QVERIFY(e.d->start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(*e.d, mode));
        Config quiet = e.d->config();
        quiet.passwordAskNote = false;
        QVERIFY(e.d->applySettings(quiet));
        if (mode == VaultMode::None) {
            return;
        }
        emit e.d->control()->ingestRequested(QStringLiteral("screen-lock-pw"), marked());
        e.secret = e.d->repository()->allEntries().back();
        if (mode == VaultMode::Master && unlock) {
            QVERIFY(unlockWith(*e.d, "master"));
        }
    }

    // Does the history file (or its WAL / shared-memory files) hold these bytes? Files are closed again at once.
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

    static QStringList marked(const QString& app = QStringLiteral("KeePassXC"))
    {
        return {QStringLiteral("text/plain"), QStringLiteral("x-kde-passwordManagerHint"),
                QStringLiteral("x-clipitto-source:") + app};
    }

    static QAction* menuAction(QMenu* menu, const QString& text)
    {
        for (QAction* a : menu->actions()) {
            if (a->text() == text) {
                return a;
            }
            if (a->menu()) {
                for (QAction* inner : a->menu()->actions()) {
                    if (inner->text() == text) {
                        return inner;
                    }
                }
            }
        }
        return nullptr;
    }

    qint64 ingestPlain(Daemon& d, const QString& text)
    {
        emit d.control()->ingestRequested(text, {QStringLiteral("text/plain")});
        return d.repository()->allEntries().back().id;
    }

    // Detection: what happens to one copy with a master password, in the open mode and with neither.
    // A detected (not marked) copy is an ordinary copy when there is no vault to keep it in (docs: passwords.md).
    void detectionCase(const QString& text, const QStringList& mimes, const QByteArray& extraConfig, VaultMode mode,
                       bool markedByManager, const QString& expectedNote)
    {
        writeConfig("password_ask_note = false\n" + extraConfig);
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, mode));
        emit d.control()->ingestRequested(text, mimes);
        auto* repo = d.repository();
        if (mode == VaultMode::None && markedByManager) {
            QCOMPARE(repo->count(), 0); // skipped: nothing stored
            QVERIFY(!d.control()->passwordsWanted());
            QVERIFY(!historyFileHolds(text.toUtf8()));
        } else if (mode == VaultMode::None) {
            QCOMPARE(repo->count(), 1); // only a guess: an ordinary copy
            QCOMPARE(repo->countSecrets(), 0);
        } else {
            QCOMPARE(repo->count(), 1);
            QCOMPARE(repo->countSecrets(), 1);
            const Entry e = repo->allEntries().back();
            QVERIFY(e.secret);
            QCOMPARE(e.content, clipitto::db::Repository::secretLabel());
            QCOMPARE(e.group, QStringLiteral("Passwords"));
            QCOMPARE(e.note, expectedNote);
            QVERIFY(repo->search(text.left(8)).empty()); // never in the search index
            QVERIFY(d.control()->passwordsWanted());
            QVERIFY(!historyFileHolds(text.toUtf8())); // sealed, never in clear
        }
    }

private slots:
    void init()
    {
        QFile::remove(options().configPath);
        QFile::remove(options().databasePath);
    }

    void theDiagnosticLogFollowsTheSetting()
    {
        DaemonOptions o = options();
        o.debugLogPath = m_dir.filePath(QStringLiteral("logs/debug.log"));
        {
            writeConfig("debug_log = true\n");
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(QFile::exists(o.debugLogPath));
            qWarning() << "a marker line for the test"; // reaches the file as any message does
            QFile file(o.debugLogPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(QString::fromUtf8(file.readAll()).contains(QLatin1String("a marker line for the test")));
            file.close(); // Windows cannot delete a file that is open

            // Turned off: the log stops and the file is deleted.
            writeConfig("debug_log = false\n");
            d.reloadConfig();
            QVERIFY(!QFile::exists(o.debugLogPath));

            // And on again: a fresh file.
            writeConfig("debug_log = true\n");
            d.reloadConfig();
            QVERIFY(QFile::exists(o.debugLogPath));
        }
        QVERIFY(QFile::exists(o.debugLogPath)); // quitting leaves it where it is

        // --verbose keeps it on whatever the setting says, and never deletes it.
        QFile::remove(o.debugLogPath);
        o.verbose = true;
        writeConfig("debug_log = false\n");
        Daemon verbose(o, std::make_unique<FakeControl>());
        QVERIFY(verbose.start() == Daemon::StartResult::Ok);
        QVERIFY(QFile::exists(o.debugLogPath));
        verbose.reloadConfig();
        QVERIFY(QFile::exists(o.debugLogPath));
    }

    void aLogFileThatCannotBeOpenedLeavesDebugLinesOff()
    {
        QFile blocker(m_dir.filePath(QStringLiteral("blocker")));
        QVERIFY(blocker.open(QIODevice::WriteOnly)); // a file where the log's folder should be
        blocker.close();
        DaemonOptions o = options();
        o.debugLogPath = m_dir.filePath(QStringLiteral("blocker/debug.log"));
        writeConfig("debug_log = true\n");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok); // not being able to write a log is not a reason not to start
        QVERIFY(!QFile::exists(o.debugLogPath));
        const QLoggingCategory category("clipitto.test");
        QVERIFY(!category.isDebugEnabled()); // asked for, failed: the debug lines are off again
    }

    void startOpensDatabase()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QString error;
        QVERIFY2(d.start(&error) == Daemon::StartResult::Ok, qPrintable(error));
        QVERIFY(d.repository());
        QVERIFY(QFile::exists(options().databasePath));
    }

    void refusedControlLeavesDatabaseAlone()
    {
        Daemon d(options(), std::make_unique<FakeControl>(ControlChannel::StartResult::AlreadyRunning));
        QString error;
        QVERIFY(d.start(&error) == Daemon::StartResult::AlreadyRunning);
        QCOMPARE(error, QStringLiteral("fake refusal"));
        QVERIFY(!d.repository());
        QVERIFY(!QFile::exists(options().databasePath));
    }

    void ingestRequestStoresText()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("from helper"), {QStringLiteral("text/plain")});
        emit d.control()->ingestRequested(QStringLiteral("secret"), {QStringLiteral("x-kde-passwordManagerHint")});
        QCOMPARE(d.repository()->count(), 1);
        QCOMPARE(d.repository()->recent(1, false).front().content, QStringLiteral("from helper"));
    }

    // Roadmap 13: --sync (the control channel's syncRequested) runs one sync through the shared folder; the daemon wires
    // the settings, the key store and the status line.
    void syncRequestWritesTheDevicesFileAndStatusSaysSo()
    {
        QTemporaryDir share;
        DaemonOptions o = options();
        o.syncKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QCOMPARE(d.status().value(QStringLiteral("sync")).toString(), QStringLiteral("off"));
        QVERIFY(!d.repository()->syncTracking());
        Config c = d.config();
        c.syncEnabled = true;
        c.syncFolder = share.path();
        c.syncDeviceName = QStringLiteral("Test PC");
        QVERIFY(d.applySettings(c));
        QVERIFY(d.repository()->syncTracking()); // deletions are remembered from now on
        QVERIFY(d.status().value(QStringLiteral("sync")).toString().startsWith(QLatin1String("on")));

        // No passphrase yet: nothing is written.
        emit d.control()->syncRequested();
        QVERIFY(QDir(share.path()).entryList({QStringLiteral("*.clipitto-sync")}).isEmpty());

        d.sync()->setKdfCost({64, 1});
        std::optional<QString> error;
        d.sync()->setPassphrase(share.path(), QByteArray("a daemon test passphrase"), false,
                                [&error](QString e, bool) { error = e; });
        QTRY_VERIFY_WITH_TIMEOUT(error.has_value(), 20000);
        QVERIFY2(error->isEmpty(), qPrintable(*error));
        emit d.control()->ingestRequested(QStringLiteral("shared text"), {QStringLiteral("text/plain")});
        QSignalSpy done(d.sync(), &clipitto::SyncService::finished);
        emit d.control()->syncRequested();
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() && !d.sync()->isRunning(), 20000);
        QCOMPARE(QDir(share.path()).entryList({QStringLiteral("*.clipitto-sync")}).size(), 1);
        QVERIFY2(d.status().value(QStringLiteral("sync")).toString().contains(QLatin1String("last sync")),
                 qPrintable(d.status().value(QStringLiteral("sync")).toString()));
        // Turned off again: tracking stops.
        c.syncEnabled = false;
        QVERIFY(d.applySettings(c));
        QVERIFY(!d.repository()->syncTracking());
    }

    void markedCopiesAreKeptOnceAMasterPasswordIsSet()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const QStringList marked = {QStringLiteral("text/plain"), QStringLiteral("x-kde-passwordManagerHint"),
                                    QStringLiteral("x-clipitto-source:KeePassXC")};
        QVERIFY(!d.control()->passwordsWanted());
        emit d.control()->ingestRequested(QStringLiteral("hunter2"), marked);
        QCOMPARE(d.repository()->count(), 0);
        QVERIFY(d.status().value(QStringLiteral("passwords")).toString().startsWith(QLatin1String("off")));

        d.passwords()->setKdfCost({64, 1});
        bool set = false;
        d.passwords()->setMasterPassword("master", [&set](bool ok) { set = ok; });
        QTRY_VERIFY(set);
        QVERIFY(d.control()->passwordsWanted());
        Config quiet = d.config();
        quiet.passwordAskNote = false; // on by default; this test is not about the name box
        QVERIFY(d.applySettings(quiet));
        emit d.control()->ingestRequested(QStringLiteral("hunter2"), marked);
        QCOMPARE(d.repository()->countSecrets(), 1);
        const Entry e = d.repository()->allEntries().back();
        QVERIFY(e.secret);
        QCOMPARE(e.note, QStringLiteral("KeePassXC"));
        QCOMPARE(d.status().value(QStringLiteral("passwords")).toString(), QStringLiteral("locked"));

        // Pasting it: on the clipboard with the marker, taken off again after password_clear_seconds.
        Config fast = d.config();
        fast.passwordClearSeconds = 1;
        QVERIFY(d.applySettings(fast));
        bool unlocked = false;
        d.passwords()->unlock("master", [&unlocked](bool ok) { unlocked = ok; });
        QTRY_VERIFY(unlocked);
        emit d.popup()->entryActivated(e);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("hunter2"));
        // Marked the platform's way (x-kde-passwordManagerHint; Windows' exclude formats).
        QVERIFY2(clipitto::ClipFilter::isSensitive(QGuiApplication::clipboard()->mimeData()->formats()),
                 qPrintable(QGuiApplication::clipboard()->mimeData()->formats().join(QLatin1Char(' '))));
        QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::clipboard()->text().isEmpty(), 3000);
        QCOMPARE(d.repository()->countSecrets(), 1); // our own paste wasn't stored again
        QCOMPARE(d.repository()->count(), 1);

        // Copied something else meanwhile: that stays.
        emit d.popup()->entryActivated(e);
        QGuiApplication::clipboard()->setText(QStringLiteral("copied after"));
        QTest::qWait(1500);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("copied after"));

        // Turned off in the settings: dropped again, and helpers are told not to read them.
        Config c = d.config();
        c.keepPasswords = false;
        QVERIFY(d.applySettings(c));
        QVERIFY(!d.control()->passwordsWanted());
        const qint64 before = d.repository()->countSecrets();
        emit d.control()->ingestRequested(QStringLiteral("other"), marked);
        QCOMPARE(d.repository()->countSecrets(), before);
    }

    // --- Auto-forget (roadmap 18) and clearing a pasted password, end to end ---

    // A history file with copies made `daysAgo` days ago, written before the daemon starts.
    void seedHistory(const QList<std::pair<QString, int>>& copies, const QString& pinned = QString())
    {
        QVERIFY(QDir().mkpath(QFileInfo(options().databasePath).absolutePath()));
        clipitto::db::Db db(options().databasePath);
        clipitto::db::configureConnection(db);
        clipitto::db::migrate(db);
        QDateTime clock = QDateTime::currentDateTimeUtc();
        clipitto::db::Repository repo(db, [&clock] { return clock; });
        for (const auto& [text, daysAgo] : copies) {
            clock = QDateTime::currentDateTimeUtc().addDays(-daysAgo);
            const qint64 id = repo.ingest(text).id;
            if (text == pinned) {
                repo.setPinned(id, true);
            }
        }
    } // the file is closed again here (Windows cannot reopen it otherwise)

    void autoForgetRunsAtStartAndIsSilent()
    {
        seedHistory({{QStringLiteral("zqxw-ancient"), 400}, {QStringLiteral("zqxw-pinned-ancient"), 400},
                     {QStringLiteral("zqxw-last-week"), 7}, {QStringLiteral("zqxw-today"), 0}},
                    QStringLiteral("zqxw-pinned-ancient"));
        writeConfig("debug_log = true\nauto_forget = [\"age 30d\"]\n");
        int toasts = 0;
        DaemonOptions o = options();
        o.debugLogPath = m_dir.filePath(QStringLiteral("debug.log"));
        o.notify = [&toasts](const QString&, const QString&) { ++toasts; return true; };
        {
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QCOMPARE(d.config().autoForget.size(), 1);
            QCOMPARE(d.repository()->count(), 3); // the ancient one went at the start
            QStringList left;
            for (const clipitto::Entry& e : d.repository()->allEntries()) {
                left << e.content;
            }
            QVERIFY(!left.contains(QStringLiteral("zqxw-ancient")));
            QVERIFY(left.contains(QStringLiteral("zqxw-pinned-ancient"))); // pinned: never
            QCOMPARE(toasts, 0); // silent: no notification
            QVERIFY(!historyFileHolds("zqxw-ancient"));
            QVERIFY(historyFileHolds("zqxw-last-week"));
        }
        // Nothing but a count in the log, never the copies.
        QFile log(o.debugLogPath);
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(log.readAll());
        log.close();
        QVERIFY(text.contains(QLatin1String("auto-forget removed 1")));
        QVERIFY(!text.contains(QLatin1String("zqxw")));
    }

    void noRulesNothingIsForgotten()
    {
        seedHistory({{QStringLiteral("ancient"), 4000}});
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(d.config().autoForget.isEmpty());
        QCOMPARE(d.runAutoForget(), qint64(0));
        QCOMPARE(d.repository()->count(), 1);
    }

    void autoForgetRunsFromTheTimer()
    {
        writeConfig("auto_forget = [\"match 1m contains \\\"Bearer\\\"\"]\n");
        qint64 shiftSecs = 0;
        DaemonOptions o = options();
        o.autoForgetIntervalMs = 50;
        o.autoForgetClock = [&shiftSecs] { return QDateTime::currentDateTimeUtc().addSecs(shiftSecs); };
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("curl -H 'Authorization: Bearer abc'"), {QStringLiteral("text/plain")});
        emit d.control()->ingestRequested(QStringLiteral("an ordinary note"), {QStringLiteral("text/plain")});
        QCOMPARE(d.repository()->count(), 2);
        QTest::qWait(300); // several timer ticks: not due yet
        QCOMPARE(d.repository()->count(), 2);
        shiftSecs = 120; // two minutes later
        QTRY_COMPARE_WITH_TIMEOUT(d.repository()->count(), 1, 5000);
        QCOMPARE(d.repository()->recent(1, false).front().content, QStringLiteral("an ordinary note"));
    }

    void autoForgetRunsWhenThePopupOpens()
    {
        writeConfig("hide_on_focus_loss = false\nauto_forget = [\"match 1m token\"]\n");
        qint64 shiftSecs = 0;
        DaemonOptions o = options();
        o.autoForgetIntervalMs = 3600 * 1000; // the timer is not what does it here
        o.autoForgetClock = [&shiftSecs] { return QDateTime::currentDateTimeUtc().addSecs(shiftSecs); };
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QString(QStringLiteral("ghp_")) + QString(36, QLatin1Char('a')),
                                          {QStringLiteral("text/plain")});
        emit d.control()->ingestRequested(QStringLiteral("an ordinary note"), {QStringLiteral("text/plain")});
        // (A detected token is an ordinary copy when there is no master password to keep it in.)
        QCOMPARE(d.repository()->count(), 2);
        QCOMPARE(d.repository()->countSecrets(), 0);
        shiftSecs = 300;
        QCOMPARE(d.repository()->count(), 2); // nothing yet: no timer tick, popup closed
        d.popup()->showPopup();
        QCOMPARE(d.repository()->count(), 1);
        d.popup()->hidePopup();
    }

    void changedRulesApplyAtOnceAndOnReload()
    {
        seedHistory({{QStringLiteral("old one"), 100}, {QStringLiteral("newer one"), 2}});
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QCOMPARE(d.repository()->count(), 2);
        Config c = d.config();
        clipitto::AutoForgetRule rule;
        rule.amount = 30;
        c.autoForget = {rule};
        QVERIFY(d.applySettings(c));
        QCOMPARE(d.repository()->count(), 1);
        // And the file is what a user would edit by hand.
        writeConfig("auto_forget = [\"age 1d\"]\n");
        d.reloadConfig();
        QCOMPARE(d.config().autoForget.size(), 1);
        QCOMPARE(d.repository()->count(), 0); // the other copy is two days old: over the new limit of one
    }

    void aBadAutoForgetRuleIsSkippedWithAWarningAndNothingElseHappens()
    {
        seedHistory({{QStringLiteral("old one"), 100}});
        writeConfig("auto_forget = [\"every day at noon\"]\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(d.config().autoForget.isEmpty());
        QCOMPARE(d.repository()->count(), 1);
    }

    // Pastes a freshly kept password; the clipboard then holds it with the markers.
    void pastePassword(Daemon& d, const QString& text)
    {
        emit d.control()->ingestRequested(text, marked());
        const clipitto::Entry e = d.repository()->allEntries().back();
        QVERIFY2(e.secret, "not kept as a password copy");
        emit d.popup()->entryActivated(e);
    }

    void aPastedPasswordIsClearedAfterTheSecondsAndNotRecorded()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        writeConfig("password_ask_note = false\npassword_clear_seconds = 1\n");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        QVERIFY(unlockWith(d, "master"));
        pastePassword(d, QStringLiteral("hunter2-clear"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("hunter2-clear"));
        QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::clipboard()->text().isEmpty(), 5000);
        QCOMPARE(d.repository()->count(), 1); // the clear is not a copy
        QCOMPARE(d.repository()->countSecrets(), 1);
        // The user's own next copy is captured as usual.
        emit d.control()->ingestRequested(QStringLiteral("my next copy"), {QStringLiteral("text/plain")});
        QCOMPARE(d.repository()->count(), 2);
    }

    void aNewerCopyIsNotClearedEvenWhenTheCaptureIsLate()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        writeConfig("password_ask_note = false\npassword_clear_seconds = 1\n");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        QVERIFY(unlockWith(d, "master"));
        pastePassword(d, QStringLiteral("hunter2-late"));
        QGuiApplication::clipboard()->setText(QStringLiteral("copied right before the clear"));
        // (the capture is told nothing: the clipboard itself decides)
        QTest::qWait(1800);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("copied right before the clear"));
    }

    void passwordClearSecondsZeroLeavesItThere()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        writeConfig("password_ask_note = false\npassword_clear_seconds = 0\n");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        QVERIFY(unlockWith(d, "master"));
        pastePassword(d, QStringLiteral("hunter2-stays"));
        QTest::qWait(1500);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("hunter2-stays"));
    }

    void aPendingPasswordIsClearedWhenTheDaemonQuits()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        writeConfig("password_ask_note = false\npassword_clear_seconds = 60\n");
        {
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(d, VaultMode::Master));
            QVERIFY(unlockWith(d, "master"));
            pastePassword(d, QStringLiteral("hunter2-quit"));
            QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("hunter2-quit"));
        }
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
    }

    void quittingLeavesANewerCopyAlone()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        writeConfig("password_ask_note = false\npassword_clear_seconds = 60\n");
        {
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(d, VaultMode::Master));
            QVERIFY(unlockWith(d, "master"));
            pastePassword(d, QStringLiteral("hunter2-quit2"));
            QGuiApplication::clipboard()->setText(QStringLiteral("my own copy"));
        }
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("my own copy"));
    }

    void askingForANameAfterCopyingAPassword()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.passwords()->setKdfCost({64, 1});
        bool set = false;
        d.passwords()->setMasterPassword("master", [&set](bool ok) { set = ok; });
        QTRY_VERIFY(set);
        Config off = d.config();
        off.passwordAskNote = false;
        off.passwordsPerNote = false; // a renamed copy stays "the same password" for this test
        QVERIFY(d.applySettings(off));
        QStringList asked;
        d.popup()->setNamePrompt([&asked](QWidget*, const QString& suggestion) {
            asked << suggestion;
            return std::optional<QString>(QStringLiteral("My bank"));
        });
        const auto copy = [&](const char* password) {
            emit d.control()->ingestRequested(QString::fromLatin1(password),
                                              {QStringLiteral("text/plain"), QStringLiteral("x-kde-passwordManagerHint"),
                                               QStringLiteral("x-clipitto-source:Safari")});
        };

        copy("pw-one"); // the option is off: nothing is asked, the note is the app
        QTest::qWait(150);
        QVERIFY(asked.isEmpty());
        QVERIFY(!d.popup()->isVisible());
        QCOMPARE(d.repository()->allEntries().back().note, QStringLiteral("Safari"));

        Config c = d.config();
        c.passwordAskNote = true;
        QVERIFY(d.applySettings(c));
        copy("pw-two");
        QTRY_COMPARE(asked, QStringList{QStringLiteral("Safari")}); // the app it came from is suggested
        QTRY_COMPARE(d.repository()->allEntries().back().note, QStringLiteral("My bank"));
        QTRY_VERIFY(!d.popup()->isVisible()); // it closes again: the focus goes back

        copy("pw-two"); // the same password again is not new: not asked
        QTest::qWait(150);
        QCOMPARE(asked.size(), 1);
    }

    // Issue #121: marking a copy by hand (context menu, group box as Ctrl+G opens it) opens the name box.
    void nameBoxAfterMarkingByHand()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.passwords()->setKdfCost({64, 1});
        bool set = false;
        d.passwords()->setMasterPassword("master", [&set](bool ok) { set = ok; });
        QTRY_VERIFY(set);
        Config c = d.config();
        c.passwordAskNote = true;
        c.passwordsPerNote = false;
        QVERIFY(d.applySettings(c));

        QStringList asked;
        QString answer = QStringLiteral("My bank");
        d.popup()->setNamePrompt([&](QWidget*, const QString& suggestion) {
            asked << suggestion;
            return std::optional<QString>(answer);
        });
        const auto ingest = [&](const QString& text) {
            emit d.control()->ingestRequested(text, {});
            return d.repository()->allEntries().back().id;
        };
        const auto menuFor = [&](qint64 id) {
            return std::unique_ptr<QMenu>(d.popup()->createContextMenu(*d.repository()->get(id), d.popup()));
        };
        const auto action = [](QMenu* menu, const QString& text) {
            for (QAction* a : menu->actions()) {
                if (a->text() == text) {
                    return a;
                }
                if (a->menu()) {
                    for (QAction* inner : a->menu()->actions()) {
                        if (inner->text() == text) {
                            return inner;
                        }
                    }
                }
            }
            return static_cast<QAction*>(nullptr);
        };
        const auto note = [&](qint64 id) { return d.repository()->get(id)->note; };

        // The context menu action.
        const qint64 one = ingest(QStringLiteral("plain one"));
        auto menu = menuFor(one);
        QVERIFY(action(menu.get(), QStringLiteral("Mark as password")));
        action(menu.get(), QStringLiteral("Mark as password"))->trigger();
        QTRY_COMPARE(asked.size(), 1);
        QTRY_COMPARE(note(one), QStringLiteral("My bank"));
        QTRY_VERIFY(!d.popup()->isVisible());
        QVERIFY(d.control()->status().value(QStringLiteral("password_feature")).toString().contains(QStringLiteral("named")));

        // The group box (Ctrl+G opens it): the Passwords group marks the copy and asks too.
        d.popup()->setGroupPrompt([](QWidget*, const QString&, const QStringList&) {
            return std::optional<QString>(QStringLiteral("Passwords"));
        });
        answer = QString(); // an empty answer keeps the default note
        const qint64 two = ingest(QStringLiteral("plain two"));
        menu = menuFor(two);
        action(menu.get(), QStringLiteral("New group…"))->trigger();
        QTRY_COMPARE(asked.size(), 2);
        QVERIFY(d.repository()->get(two)->secret);
        QTRY_VERIFY(!d.popup()->isVisible());
        QVERIFY(note(two) != QStringLiteral("My bank"));

        answer = QStringLiteral("Mail");
        const qint64 three = ingest(QStringLiteral("plain three"));
        menu = menuFor(three);
        action(menu.get(), QStringLiteral("New group…"))->trigger();
        QTRY_COMPARE(asked.size(), 3);
        QTRY_COMPARE(note(three), QStringLiteral("Mail"));

        // Off in the settings: not asked.
        Config off = d.config();
        off.passwordAskNote = false;
        QVERIFY(d.applySettings(off));
        const qint64 four = ingest(QStringLiteral("plain four"));
        menu = menuFor(four);
        action(menu.get(), QStringLiteral("Mark as password"))->trigger();
        QTest::qWait(150);
        QCOMPARE(asked.size(), 3);
        QVERIFY(d.repository()->get(four)->secret);
    }

    void nameBoxIsNotAskedForAPasswordAlreadyKept()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        d.passwords()->setKdfCost({64, 1});
        bool set = false;
        d.passwords()->setMasterPassword("master", [&set](bool ok) { set = ok; });
        QTRY_VERIFY(set);
        Config c = d.config();
        c.passwordAskNote = true;
        c.passwordsPerNote = false;
        QVERIFY(d.applySettings(c));
        int asked = 0;
        d.popup()->setNamePrompt([&](QWidget*, const QString&) {
            ++asked;
            return std::optional<QString>(QString());
        });
        emit d.control()->ingestRequested(QStringLiteral("same-secret"), {QStringLiteral("x-kde-passwordManagerHint"), QStringLiteral("x-clipitto-source:App")});
        QTRY_COMPARE(asked, 1);
        emit d.control()->ingestRequested(QStringLiteral("same-secret"), {});
        const qint64 plain = d.repository()->allEntries().back().id;
        QVERIFY(plain > 0);
        std::unique_ptr<QMenu> menu(d.popup()->createContextMenu(*d.repository()->get(plain), d.popup()));
        for (QAction* a : menu->actions()) {
            if (a->text() == QLatin1String("Mark as password")) {
                a->trigger();
            }
        }
        QTest::qWait(150);
        QCOMPARE(asked, 1);
    }

    void clipboardChangesAreCaptured()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QGuiApplication::clipboard()->setText(QStringLiteral("copied in some app"));
        QTRY_COMPARE(d.repository()->count(), 1);
        QCOMPARE(d.repository()->recent(1, false).front().content, QStringLiteral("copied in some app"));
    }

    void statusReportsState()
    {
        writeConfig("max_history = 250\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("one"), {});

        const QVariantMap s = d.control()->status();
        QCOMPARE(s.value(QStringLiteral("items")).toLongLong(), 1);
        QCOMPARE(s.value(QStringLiteral("max_history")).toInt(), 250);
        QCOMPARE(s.value(QStringLiteral("backend")).toString(), QStringLiteral("qt-clipboard"));
        QCOMPARE(s.value(QStringLiteral("database")).toString(), options().databasePath);
    }

    void reloadAppliesLowerLimit()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        for (int i = 0; i < 20; ++i) {
            emit d.control()->ingestRequested(QStringLiteral("bulk %1").arg(i), {});
        }
        writeConfig("max_history = 10\n");
        emit d.control()->reloadRequested();
        QCOMPARE(d.config().maxHistory, 10);
        QCOMPARE(d.repository()->countUnsaved(), 10);
    }

    void toggleShowsAndHidesPopup()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QSignalSpy visibility(d.control(), &ControlChannel::visibilityChanged);

        emit d.control()->toggleRequested(QString());
        QVERIFY(d.popup()->isVisible());
        QVERIFY(d.control()->status().value(QStringLiteral("visible")).toBool());
        emit d.control()->toggleRequested(QString());
        QVERIFY(!d.popup()->isVisible());

        emit d.control()->showRequested(QString());
        emit d.control()->showRequested(QString()); // idempotent
        QVERIFY(d.popup()->isVisible());
        emit d.control()->hideRequested();
        QVERIFY(!d.popup()->isVisible());

        QCOMPARE(visibility.size(), 4);
        QCOMPARE(visibility.at(0).at(0).toBool(), true);
        QCOMPARE(visibility.at(3).at(0).toBool(), false);
    }

    void activatingAnEntryCopiesItBack()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("older"), {});
        emit d.control()->ingestRequested(QStringLiteral("newer"), {});

        emit d.control()->showRequested(QString());
        auto* model = d.popup()->model();
        emit d.popup()->view()->activateRequested(
            model->findEntry(d.repository()->search(QStringLiteral("older")).front().id));

        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("older"));
        QVERIFY(!d.popup()->isVisible());
        QCoreApplication::processEvents(); // deliver the clipboard-change echo
        QCOMPARE(d.repository()->count(), 2); // the echo is not a new copy
        QCOMPARE(d.repository()->recent(1, false).front().content, QStringLiteral("older"));
    }

    void editedTextReachesTheClipboardOnlyForTheNewestCopy()
    {
        QStringList notes;
        DaemonOptions o = options();
        o.notify = [&](const QString&, const QString& body) {
            notes << body;
            return true;
        };
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("older"), {});
        emit d.control()->ingestRequested(QStringLiteral("newer"), {});
        const QString untouched = QGuiApplication::clipboard()->text();
        const qint64 count = d.repository()->count();
        const auto olderId = d.repository()->search(QStringLiteral("older")).front().id;
        const auto newerId = d.repository()->search(QStringLiteral("newer")).front().id;

        // Not the newest: stored, clipboard and notification left alone.
        d.repository()->setContent(olderId, QStringLiteral("older edited"), 1024);
        emit d.popup()->textEdited(olderId, QStringLiteral("older edited"));
        QCOMPARE(QGuiApplication::clipboard()->text(), untouched);
        QVERIFY(notes.isEmpty());

        // The newest: the clipboard follows, one notification, and no new entry from the echo.
        d.repository()->setContent(newerId, QStringLiteral("newer edited"), 1024);
        emit d.popup()->textEdited(newerId, QStringLiteral("newer edited"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("newer edited"));
        QCOMPARE(notes.size(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(d.repository()->count(), count);

        // No notification service: the popup says it instead of failing silently.
        DaemonOptions o2 = options();
        o2.databasePath = m_dir.filePath(QStringLiteral("data/other.db"));
        o2.notify = [](const QString&, const QString&) { return false; };
        Daemon d2(o2, std::make_unique<FakeControl>());
        QVERIFY(d2.start() == Daemon::StartResult::Ok);
        emit d2.control()->ingestRequested(QStringLiteral("only"), {});
        const auto onlyId = d2.repository()->search(QStringLiteral("only")).front().id;
        emit d2.popup()->textEdited(onlyId, QStringLiteral("only edited"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("only edited"));
    }

    void integrationActivatesPopupAndOwnsClipboard()
    {
        auto integration = std::make_unique<FakeIntegration>(true);
        FakeIntegration* fake = integration.get();
        Daemon d(options(), std::make_unique<FakeControl>(), std::move(integration));
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("from history"), {});
        QGuiApplication::clipboard()->setText(QStringLiteral("sentinel"));
        QCoreApplication::processEvents();

        emit d.control()->showRequested(QString());
        QCOMPARE(fake->activations, 1);
        emit d.popup()->view()->activateRequested(d.popup()->model()->findEntry(d.repository()->search(QStringLiteral("from history")).front().id));
        QCOMPARE(fake->clipboardTexts, QStringList{QStringLiteral("from history")});
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("sentinel")); // left to the shell
        QVERIFY(d.control()->status().value(QStringLiteral("desktop_integration")).toString().contains(
            QLatin1String("connected")));
    }

    void unavailableIntegrationFallsBackToQtClipboard()
    {
        auto integration = std::make_unique<FakeIntegration>(false);
        FakeIntegration* fake = integration.get();
        Daemon d(options(), std::make_unique<FakeControl>(), std::move(integration));
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("local copy"), {});
        emit d.control()->showRequested(QString());
        QCOMPARE(fake->activations, 0);
        emit d.popup()->view()->activateRequested(d.popup()->model()->firstEntry());
        QVERIFY(fake->clipboardTexts.isEmpty());
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("local copy"));
    }

    void searchPasteAndReopenShowsTheHistoryRight()
    {
        // try0, try1, try2 copied (try0 also in a group); search "try0", Enter (copy + paste),
        // reopen: try0, being in a group, was not moved to the top by the paste (Recent is not
        // reshuffled by using a filed copy), is listed once, and the history holds three copies.
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        for (const char* text : {"try0", "try1", "try2"}) {
            emit d.control()->ingestRequested(QString::fromLatin1(text), {});
        }
        const qint64 try0 = d.repository()->search(QStringLiteral("try0")).front().id;
        d.repository()->setGroup(try0, QStringLiteral("Work"));

        auto* popup = d.popup();
        emit d.control()->showRequested(QString());
        QTest::keyClicks(popup->searchBar(), QStringLiteral("try0"));
        QTest::keyClick(popup->searchBar(), Qt::Key_Return);
        QVERIFY(!popup->isVisible());
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("try0"));
        QTest::qWait(100); // the clipboard change comes back through capture: no new entry

        emit d.control()->showRequested(QString());
        QVERIFY(popup->searchBar()->text().isEmpty());
        QStringList rows; // top-level copies, in order
        for (int i = 0; i < popup->model()->rowCount(); ++i) {
            if (const auto* e = popup->model()->entryAt(popup->model()->index(i, 0))) {
                rows << e->content;
            }
        }
        QCOMPARE(rows, (QStringList{"try2", "try1", "try0"}));
        QCOMPARE(d.repository()->count(), 3);

        // Copying try0 again (another app, later than our paste's echo) keeps one entry and
        // makes it the latest.
        QTest::qWait(int(clipitto::ClipFilter::ownWriteWindowMs));
        emit d.control()->ingestRequested(QStringLiteral("try1"), {});
        emit d.control()->ingestRequested(QStringLiteral("try0"), {});
        QCOMPARE(d.repository()->count(), 3);
        QCOMPARE(d.repository()->recentUnpinned(10).front().content, QStringLiteral("try0"));
    }

    void picturesAreStoredAndPastedBack()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QImage picture(200, 100, QImage::Format_RGB32);
        picture.fill(QColor(200, 40, 40));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");

        // The GNOME extension's IngestImage: decoded off the GUI thread, then stored.
        emit d.control()->ingestImageRequested(png, QStringLiteral("image/png"), {QStringLiteral("image/png")});
        QTRY_COMPARE(d.repository()->countImages(), 1);
        QCOMPARE(d.control()->status().value(QStringLiteral("images")).toInt(), 1);
        emit d.control()->ingestRequested(QStringLiteral("a newer text"), {});

        // Choosing it in the popup puts the picture back on the clipboard...
        emit d.control()->showRequested(QString());
        QModelIndex image;
        for (int i = 0; i < d.popup()->model()->rowCount(); ++i) {
            const QModelIndex index = d.popup()->model()->index(i, 0);
            if (const auto* e = d.popup()->model()->entryAt(index); e && e->image) {
                image = index;
            }
        }
        QVERIFY(image.isValid());
        QCOMPARE(image.data(HistoryModel::TitleRole).toString(), QStringLiteral("Image 200×100"));
        QVERIFY(!image.data(HistoryModel::ThumbnailRole).value<QPixmap>().isNull());
        emit d.popup()->view()->activateRequested(image);
#if defined(_WIN32)
        // Windows: a bitmap for every app, plus the standard "PNG" format.
        const QImage pasted = QGuiApplication::clipboard()->image();
        QVERIFY(QGuiApplication::clipboard()->mimeData()->hasFormat(
            QStringLiteral("application/x-qt-windows-mime;value=\"PNG\"")));
#else
        // Linux: the stored PNG as it is.
        const QImage pasted =
            QImage::fromData(QGuiApplication::clipboard()->mimeData()->data(QStringLiteral("image/png")), "PNG");
#endif
        QCOMPARE(pasted.size(), QSize(200, 100));
        QCOMPARE(pasted.pixelColor(10, 10), QColor(200, 40, 40));

        // ...and that clipboard change is not a second copy; the picture is the latest.
        QTest::qWait(300);
        QCOMPARE(d.repository()->countImages(), 1);
        QCOMPARE(d.repository()->count(), 2);
        QVERIFY(d.repository()->recentUnpinned(1).front().image);
    }

    void picturesArePastedThroughTheShellExtension()
    {
        auto integration = std::make_unique<FakeIntegration>(true);
        FakeIntegration* fake = integration.get();
        Daemon d(options(), std::make_unique<FakeControl>(), std::move(integration));
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QImage picture(30, 30, QImage::Format_RGB32);
        picture.fill(Qt::green);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
        emit d.control()->ingestImageRequested(png, QStringLiteral("image/png"), {});
        QTRY_COMPARE(d.repository()->countImages(), 1);

        emit d.control()->showRequested(QString());
        emit d.popup()->view()->activateRequested(d.popup()->model()->firstEntry());
        QCOMPARE(fake->clipboardImages.size(), 1);
        QCOMPARE(QImage::fromData(fake->clipboardImages.front(), "PNG").pixelColor(1, 1), QColor(Qt::green));
        QVERIFY(fake->clipboardTexts.isEmpty());
    }

    void picturesCanBeTurnedOff()
    {
        writeConfig("max_images = 0\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QImage picture(10, 10, QImage::Format_RGB32);
        picture.fill(Qt::red);
        QGuiApplication::clipboard()->setImage(picture); // copied in another app
        QTest::qWait(300);
        QCOMPARE(d.repository()->countImages(), 0);
    }

    void latestAfterPasteCanBeTurnedOff()
    {
        writeConfig("latest_after_paste = false\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        for (const char* text : {"try0", "try1", "try2"}) {
            emit d.control()->ingestRequested(QString::fromLatin1(text), {});
        }
        emit d.control()->showRequested(QString());
        QTest::keyClicks(d.popup()->searchBar(), QStringLiteral("try0"));
        QTest::keyClick(d.popup()->searchBar(), Qt::Key_Return);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("try0")); // still copied
        QTest::qWait(100); // the clipboard's echo must not move it either
        QStringList order;
        for (const auto& e : d.repository()->recentUnpinned(10)) {
            order << e.content;
        }
        QCOMPARE(order, (QStringList{"try2", "try1", "try0"}));
    }

    void lastPopupPositionSurvivesARestart()
    {
        writeConfig("popup_position = \"last\"\n");
        {
            Daemon d(options(), std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            emit d.control()->showRequested(QString());
            d.popup()->resize(200, 150);
            d.popup()->move(40, 30); // e.g. dragged there
            emit d.control()->hideRequested();
        }
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QCOMPARE(d.popup()->lastPosition(), std::optional<QPoint>(QPoint(40, 30)));
        d.popup()->resize(200, 150);
        emit d.control()->showRequested(QString());
        QCOMPARE(d.popup()->pos(), QPoint(40, 30));
    }

    void pasteOnEnterUsesIntegration()
    {
        writeConfig("paste_on_enter = true\npaste_keys = \"shift+insert\"\n");
        auto integration = std::make_unique<FakeIntegration>(true);
        FakeIntegration* fake = integration.get();
        Daemon d(options(), std::make_unique<FakeControl>(), std::move(integration));
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("paste me"), {});
        emit d.control()->showRequested(QString());
        emit d.popup()->view()->activateRequested(d.popup()->model()->firstEntry());
        QVERIFY(fake->pastes.isEmpty()); // only after the popup is gone
        QTRY_COMPARE(fake->pastes, QList<PasteKeys>{PasteKeys::ShiftInsert});
    }

    // The keys typed for a paste into `target`, through the synthetic path (no shell integration).
    QList<PasteKeys> typedFor(const PasteTarget& target, const QByteArray& config)
    {
        writeConfig("paste_on_enter = true\n" + config);
        QList<PasteKeys> typed;
        DaemonOptions o = options();
        o.pasteTarget = [&target] { return target; };
        o.syntheticPaste = [&typed](PasteKeys keys) {
            typed << keys;
            return true;
        };
        Daemon d(o, std::make_unique<FakeControl>());
        if (d.start() != Daemon::StartResult::Ok) {
            return {};
        }
        emit d.control()->ingestRequested(QStringLiteral("paste me"), {});
        emit d.control()->showRequested(QString());
        emit d.popup()->view()->activateRequested(d.popup()->model()->firstEntry());
        (void)QTest::qWaitFor([&typed] { return !typed.isEmpty(); }, 10000); // empty = nothing was typed
        return typed;
    }

    void pasteUsesTheKeysOfTheProgramThatGetsIt()
    {
        const auto known = [](const char* id) { return PasteTarget::known({QString::fromLatin1(id)}); };
        // A terminal gets ctrl+shift+v, an ordinary program the global keys.
        QCOMPARE(typedFor(known("kitty"), ""), QList<PasteKeys>{PasteKeys::CtrlShiftV});
        QCOMPARE(typedFor(known("notepad.exe"), ""), QList<PasteKeys>{PasteKeys::CtrlV});
        QCOMPARE(typedFor(known("notepad.exe"), "paste_keys = \"shift+insert\"\n"), QList<PasteKeys>{PasteKeys::ShiftInsert});
        // Your rule wins over the built-in one, and names a program the built-ins do not know.
        QCOMPARE(typedFor(known("kitty"), "paste_keys_apps = [\"kitty = shift+insert\"]\n"), QList<PasteKeys>{PasteKeys::ShiftInsert});
        QCOMPARE(typedFor(known("Code"), "paste_keys_apps = [\"code = ctrl+shift+v\"]\n"), QList<PasteKeys>{PasteKeys::CtrlShiftV});
        // The switch off: the global keys for everyone, terminals and your rules included.
        QCOMPARE(typedFor(known("kitty"), "paste_keys_per_app = false\npaste_keys_apps = [\"kitty = shift+insert\"]\n"),
                 QList<PasteKeys>{PasteKeys::CtrlV});
    }

    void pasteFallsBackToTheGlobalKeysWhereTheProgramIsNotKnown()
    {
        // Unsupported platform (Wayland without the extension, macOS) and no window remembered.
        QCOMPARE(typedFor(PasteTarget::unsupported(), "paste_keys = \"shift+insert\"\n"), QList<PasteKeys>{PasteKeys::ShiftInsert});
        QCOMPARE(typedFor(PasteTarget::unknown(), "paste_keys_apps = [\"kitty = ctrl+shift+v\"]\n"), QList<PasteKeys>{PasteKeys::CtrlV});
    }

    void shellIntegrationIsAskedForTheProgramWhenThePopupOpensAndPastesItsKeys()
    {
        writeConfig("paste_on_enter = true\n");
        auto integration = std::make_unique<FakeIntegration>(true);
        FakeIntegration* fake = integration.get();
        fake->target = PasteTarget::known({QStringLiteral("org.gnome.Terminal"), QStringLiteral("gnome-terminal-server")});
        Daemon d(options(), std::make_unique<FakeControl>(), std::move(integration));
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("paste me"), {});
        QCOMPARE(fake->targetAsks, 0);
        emit d.control()->showRequested(QString());
        QCOMPARE(fake->targetAsks, 1); // before the popup takes the focus
        emit d.popup()->view()->activateRequested(d.popup()->model()->firstEntry());
        QTRY_COMPARE(fake->pastes, QList<PasteKeys>{PasteKeys::CtrlShiftV});

        // An older extension cannot say: the global keys.
        fake->pastes.clear();
        fake->target = PasteTarget::unsupported();
        emit d.control()->showRequested(QString());
        emit d.popup()->view()->activateRequested(d.popup()->model()->firstEntry());
        QTRY_COMPARE(fake->pastes, QList<PasteKeys>{PasteKeys::CtrlV});
    }

    void captureBackendFollowsIntegration()
    {
        DaemonOptions o = options();
        o.session.display = clipitto::SessionEnv::Display::Wayland;
        o.session.isGnome = true;
        o.wlPasteProgram = QStringLiteral("/nonexistent/wl-paste"); // no data-control either
        auto integration = std::make_unique<FakeIntegration>(false);
        FakeIntegration* fake = integration.get();
        Daemon d(o, std::make_unique<FakeControl>(), std::move(integration));
        QVERIFY(d.start() == Daemon::StartResult::Ok);

        const auto backend = [&] { return d.control()->status().value(QStringLiteral("backend")).toString(); };
        const auto degraded = [&] { return d.control()->status().value(QStringLiteral("backend_degraded")).toBool(); };
        QCOMPARE(backend(), QStringLiteral("qt-clipboard"));
        QVERIFY(degraded());

        fake->setAvailable(true); // extension enabled
        QCOMPARE(backend(), QStringLiteral("gnome-ext"));
        QVERIFY(!degraded());

        fake->setAvailable(false); // shell restarted / extension disabled
        QCOMPARE(backend(), QStringLiteral("qt-clipboard"));
    }

    void syntheticPasteNeedsADisplay()
    {
#if defined(Q_OS_WIN)
        QSKIP("SendInput needs no display connection");
#elif defined(Q_OS_MACOS)
        // The real call presses ⌘V into whatever app has focus when the Accessibility
        // permission is granted (it was, on a CI runner): never from a test.
        QSKIP("macOS pastes with real key events");
#endif
        QVERIFY(!clipitto::pasteWithSyntheticKeys(PasteKeys::CtrlV)); // offscreen: no X display
    }

    void applySettingsSavesAndApplies()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        Config c = d.config();
        c.theme = clipitto::Theme::Dark;
        c.previewLines = 3;
        QString error;
        QVERIFY2(d.applySettings(c, &error), qPrintable(error));
        QVERIFY(Config::load(options().configPath) == c); // persisted
        QVERIFY(d.config() == c); // and live
        QCOMPARE(QApplication::style()->name().toLower(), QStringLiteral("fusion"));
        QCOMPARE(QApplication::palette().color(QPalette::Window).lightness() < 128, true);

        c.theme = clipitto::Theme::Light;
        QVERIFY(d.applySettings(c));
        QCOMPARE(QApplication::palette().color(QPalette::Window).lightness() > 128, true);
    }

    void gnomeShortcutFollowsTheSetting()
    {
#ifndef CLIPITTO_FAKE_GSETTINGS
        QSKIP("needs the POSIX fake gsettings");
#else
        QTemporaryDir store;
        qputenv("FAKE_GSETTINGS_DIR", QFile::encodeName(store.path()));
        DaemonOptions o = options();
        o.session.isGnome = true;
        o.gsettingsProgram = QStringLiteral(CLIPITTO_FAKE_GSETTINGS);
        writeConfig("hotkey = \"Ctrl+`\"\n");
        const QString entry = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:"
                                             "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipitto/");
        QFile staleBinding(store.filePath(QString(entry + QStringLiteral(".binding"))
                                              .replace(QLatin1Char('/'), QLatin1Char('_'))
                                              .replace(QLatin1Char(':'), QLatin1Char('_'))));
        QVERIFY(staleBinding.open(QIODevice::WriteOnly));
        staleBinding.write("<Control><Alt>v\n");
        staleBinding.close();

        Daemon d(o, std::make_unique<FakeControl>());
        QSignalSpy applied(&d, &Daemon::shortcutApplied);
        QVERIFY(d.start() == Daemon::StartResult::Ok);

        const auto stored = [&](const QString& key) {
            QFile f(store.filePath(QString(key).replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char(':'), QLatin1Char('_'))));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
        };
        QTRY_COMPARE(applied.count(), 1);
        QCOMPARE(stored(entry + QStringLiteral(".binding")), QStringLiteral("<Control>grave"));

        Config c = d.config();
        c.hotkey = QStringLiteral("Ctrl+`");
        QVERIFY(d.applySettings(c));
        QTRY_COMPARE(applied.count(), 2);
        QCOMPARE(stored(entry + QStringLiteral(".binding")), QStringLiteral("<Control>grave"));
        QVERIFY(stored(entry + QStringLiteral(".command")).endsWith(QLatin1String("--toggle")));
        QVERIFY(stored(QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybindings"))
                    .contains(QLatin1String("/custom-keybindings/clipitto/")));
        QCOMPARE(d.status().value(QStringLiteral("shortcut")).toString(), QStringLiteral("Ctrl+` (GNOME)"));

        // Editing the file and reloading applies a changed shortcut too.
        writeConfig("hotkey = \"Ctrl+Alt+V\"\n");
        emit d.control()->reloadRequested();
        QTRY_COMPARE(applied.count(), 3);
        QCOMPARE(stored(entry + QStringLiteral(".binding")), QStringLiteral("<Control><Alt>v"));
#endif
    }

    void startupDoesNotRewriteAnUnchangedShortcut()
    {
#if defined(_WIN32)
        QSKIP("needs the POSIX fake gsettings");
#else
        QTemporaryDir store;
        qputenv("FAKE_GSETTINGS_DIR", QFile::encodeName(store.path()));
        const QString log = store.filePath(QStringLiteral("calls.log"));
        DaemonOptions o = options();
        o.session.isGnome = true;
        o.gsettingsProgram = QStringLiteral(CLIPITTO_FAKE_GSETTINGS);
        writeConfig("hotkey = \"Ctrl+`\"\n");
        {
            Daemon first(o, std::make_unique<FakeControl>());
            QSignalSpy applied(&first, &Daemon::shortcutApplied);
            QVERIFY(first.start() == Daemon::StartResult::Ok);
            QTRY_COMPARE(applied.count(), 1);
        }

        qputenv("FAKE_GSETTINGS_LOG", QFile::encodeName(log));
        Daemon second(o, std::make_unique<FakeControl>());
        QSignalSpy applied(&second, &Daemon::shortcutApplied);
        QVERIFY(second.start() == Daemon::StartResult::Ok);
        QTRY_COMPARE(applied.count(), 1);
        qunsetenv("FAKE_GSETTINGS_LOG");
        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(calls.readAll());
        QVERIFY2(text.startsWith(QLatin1String("get ")), qPrintable(text));
        QVERIFY2(!text.contains(QLatin1String("\nset ")) && !text.startsWith(QLatin1String("set ")), qPrintable(text));
        QCOMPARE(second.status().value(QStringLiteral("shortcut")).toString(), QStringLiteral("Ctrl+` (GNOME)"));
#endif
    }

    void aReloadDuringStartupWins()
    {
#if defined(_WIN32)
        QSKIP("needs the POSIX fake gsettings");
#else
        QTemporaryDir store;
        qputenv("FAKE_GSETTINGS_DIR", QFile::encodeName(store.path()));
        DaemonOptions o = options();
        o.session.isGnome = true;
        o.gsettingsProgram = QStringLiteral(CLIPITTO_FAKE_GSETTINGS);
        writeConfig("hotkey = \"Ctrl+Alt+V\"\n");
        Daemon d(o, std::make_unique<FakeControl>());
        QSignalSpy applied(&d, &Daemon::shortcutApplied);
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        writeConfig("hotkey = \"Ctrl+`\"\n");
        emit d.control()->reloadRequested(); // queued behind the start's update

        // Only the newest request reports; the older result is dropped.
        QTRY_COMPARE(applied.count(), 1);
        QTest::qWait(200);
        QCOMPARE(applied.count(), 1);
        const QString entry = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:"
                                             "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipitto/");
        QFile binding(store.filePath(QString(entry + QStringLiteral(".binding"))
                                         .replace(QLatin1Char('/'), QLatin1Char('_'))
                                         .replace(QLatin1Char(':'), QLatin1Char('_'))));
        QVERIFY(binding.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(binding.readAll()).trimmed(), QStringLiteral("<Control>grave"));
        QCOMPARE(d.status().value(QStringLiteral("shortcut")).toString(), QStringLiteral("Ctrl+` (GNOME)"));
#endif
    }

    void nonGnomeSessionsReportTheManualShortcut()
    {
        DaemonOptions o = options();
        o.session.isGnome = false;
        o.gsettingsProgram = QStringLiteral("/nonexistent/gsettings");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        Config c = d.config();
        c.hotkey = QStringLiteral("Ctrl+`");
        QVERIFY(d.applySettings(c));
        // The bogus gsettings path would make any attempt fail with "not set: ...".
        const QString shortcut = d.status().value(QStringLiteral("shortcut")).toString();
        QVERIFY2(shortcut.startsWith(QLatin1String("not set automatically")), qPrintable(shortcut));
        QVERIFY(shortcut.contains(QLatin1String("clipitto --toggle")));
    }

    void macSessionsReportTheirPermissionAndLoginItem()
    {
        QTemporaryDir agents;
        DaemonOptions o = options();
        o.session.display = SessionEnv::Display::MacOS;
        o.loginItemPath = agents.filePath(QStringLiteral("agent.plist"));
        o.gsettingsProgram = QStringLiteral("/nonexistent/gsettings");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QCOMPARE(d.status().value(QStringLiteral("session")).toString(), QStringLiteral("macos"));
        // The hotkey is registered by main.cpp, not announced as "bind it yourself".
        QVERIFY(!d.status().contains(QStringLiteral("shortcut")));
        // Auto-paste needs the Accessibility permission: say whether it is there.
        QVERIFY(d.status().contains(QStringLiteral("accessibility")));
        QCOMPARE(d.status().value(QStringLiteral("login_item")).toString(), QStringLiteral("off"));
        QVERIFY(QFile(o.loginItemPath).open(QIODevice::WriteOnly)); // what Settings' checkbox writes
        QCOMPARE(d.status().value(QStringLiteral("login_item")).toString(), QStringLiteral("on"));
    }

    void otherSessionsDoNotMentionMacThings()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(!d.status().contains(QStringLiteral("accessibility")));
        QVERIFY(!d.status().contains(QStringLiteral("login_item")));
    }

    void windowsSessionsLeaveTheShortcutToTheHotkey()
    {
        // main.cpp registers the hotkey and reports "hotkey"; the daemon adds nothing.
        DaemonOptions o = options();
        o.session.display = SessionEnv::Display::Windows;
        o.gsettingsProgram = QStringLiteral("/nonexistent/gsettings");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(!d.status().contains(QStringLiteral("shortcut")));
    }

    // --- Encrypted history (fake keyring) --------------------------------------------

    DaemonOptions encryptedOptions(const std::shared_ptr<FakeKeyStore>& keys) const
    {
        DaemonOptions o = options();
        o.keyStore = keys;
        return o;
    }

    void encryptionAtStartupAndRestart()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
        auto keys = std::make_shared<FakeKeyStore>();
        writeConfig("encrypt_history = true\n");
        {
            Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            emit d.control()->ingestRequested(QStringLiteral("secret token abc123"), {});
            QVERIFY(d.isHistoryEncrypted());
            QCOMPARE(d.control()->status().value(QStringLiteral("encryption")).toString(),
                     QStringLiteral("on (system keyring)"));
        }
        QVERIFY(keys->stored.has_value());
        QCOMPARE(clipitto::db::probeFile(options().databasePath), clipitto::db::FileState::Encrypted);
        QFile db(options().databasePath);
        QVERIFY(db.open(QIODevice::ReadOnly));
        QVERIFY(!db.readAll().contains("abc123"));

        Daemon again(encryptedOptions(keys), std::make_unique<FakeControl>()); // same key: opens
        QVERIFY(again.start() == Daemon::StartResult::Ok);
        QCOMPARE(again.repository()->search(QStringLiteral("abc123")).size(), 1u);
    }

    void switchingAtRuntimeConvertsInTheBackground()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
        auto keys = std::make_shared<FakeKeyStore>();
        Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        emit d.control()->ingestRequested(QStringLiteral("before"), {});
        QSignalSpy finished(&d, &Daemon::encryptionFinished);

        Config c = d.config();
        c.encryptHistory = true;
        QVERIFY(d.applySettings(c));
        QVERIFY(d.isConverting());
        QVERIFY(d.popup()->isBusy());
        emit d.control()->ingestRequested(QStringLiteral("during"), {}); // waits in memory
        QCOMPARE(d.control()->status().value(QStringLiteral("encryption")).toString(), QStringLiteral("converting"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QVERIFY(!d.popup()->isBusy());
        QCOMPARE(clipitto::db::probeFile(options().databasePath), clipitto::db::FileState::Encrypted);
        QCOMPARE(d.repository()->count(), 2); // the copy made during the conversion was kept

        c.encryptHistory = false;
        QVERIFY(d.applySettings(c));
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(clipitto::db::probeFile(options().databasePath), clipitto::db::FileState::Plain);
        QVERIFY(!keys->stored.has_value()); // the key is dropped with the encryption
        QCOMPARE(d.control()->status().value(QStringLiteral("encryption")).toString(), QStringLiteral("off"));
    }

    void encryptedHistoryWithoutItsKeyIsLeftAlone()
    {
        if (!clipitto::db::encryptionSupported()) {
            QSKIP("built without CLIPITTO_ENCRYPTION");
        }
        auto keys = std::make_shared<FakeKeyStore>();
        writeConfig("encrypt_history = true\n");
        {
            Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
        }
        keys->stored.reset(); // e.g. a different user account, or the keyring was reset
        Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
        QString error;
        QVERIFY(d.start(&error) == Daemon::StartResult::Failed);
        QVERIFY2(error.contains(QLatin1String("encrypted")), qPrintable(error));
        QCOMPARE(clipitto::db::probeFile(options().databasePath), clipitto::db::FileState::Encrypted);
    }

    void failedSwitchRevertsTheSetting()
    {
        auto keys = std::make_shared<FakeKeyStore>();
        keys->failWrites = true;
        Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QString shown;
        d.popup()->setNotify([&](QWidget*, const QString& message) { shown = message; });
        QSignalSpy finished(&d, &Daemon::encryptionFinished);
        Config c = d.config();
        c.encryptHistory = true;
        QVERIFY(d.applySettings(c));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), false);
        QVERIFY(shown.contains(QLatin1String("keyring refused")));
        QVERIFY(!d.config().encryptHistory); // live setting follows the file...
        QVERIFY(!Config::load(options().configPath).encryptHistory); // ...and so does the saved one
        QCOMPARE(clipitto::db::probeFile(options().databasePath), clipitto::db::FileState::Plain);
    }

    void keyringThatLosesTheKeyBlocksEncryption()
    {
        auto keys = std::make_shared<FakeKeyStore>();
        keys->loseWrites = true;
        Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QString shown;
        d.popup()->setNotify([&](QWidget*, const QString& message) { shown = message; });
        Config c = d.config();
        c.encryptHistory = true;
        QVERIFY(d.applySettings(c));
        QVERIFY(!d.isHistoryEncrypted());
        QVERIFY(shown.contains(QLatin1String("did not keep the key")));
        QCOMPARE(clipitto::db::probeFile(options().databasePath), clipitto::db::FileState::Plain);
    }

    void startupWarnsWhenEncryptionCouldNotBeApplied()
    {
        auto keys = std::make_shared<FakeKeyStore>();
        keys->isAvailable = false;
        writeConfig("encrypt_history = true\n");
        Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QStringList shown;
        d.popup()->setNotify([&](QWidget*, const QString& message) { shown << message; });
        emit d.control()->showRequested(QString());
        QTRY_COMPARE(shown.size(), 1);
        QVERIFY(shown.front().contains(QLatin1String("NOT encrypted")));
        emit d.control()->hideRequested();
        emit d.control()->showRequested(QString()); // only once
        QTest::qWait(50);
        QCOMPARE(shown.size(), 1);
    }

    void historyFileIsPrivate()
    {
#ifdef Q_OS_WIN
        QSKIP("Windows relies on the profile folder's ACL");
#else
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const auto perms = QFile::permissions(options().databasePath);
        QVERIFY(perms & QFileDevice::ReadOwner);
        QVERIFY(!(perms & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
#endif
    }

    void noKeyringKeepsTheHistoryPlainAndSaysWhy()
    {
        auto keys = std::make_shared<FakeKeyStore>();
        keys->isAvailable = false;
        writeConfig("encrypt_history = true\n");
        Daemon d(encryptedOptions(keys), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(!d.isHistoryEncrypted());
        QVERIFY(d.control()->status().value(QStringLiteral("encryption")).toString().startsWith(
            QLatin1String("off (problem:")));
        QVERIFY(!d.encryptionAvailable());
    }

    void startPrunesToConfiguredLimit()
    {
        {
            Daemon d(options(), std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            for (int i = 0; i < 30; ++i) {
                emit d.control()->ingestRequested(QStringLiteral("old %1").arg(i), {});
            }
        }
        writeConfig("max_history = 12\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QCOMPARE(d.repository()->countUnsaved(), 12);
    }

    // --- Detection: a master password, "use without one", and neither ---

    void aMarkedCopyIsKeptSealedOrSkipped_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("master password") << int(VaultMode::Master);
        QTest::newRow("use without one") << int(VaultMode::Open);
        QTest::newRow("neither") << int(VaultMode::None);
    }
    void aMarkedCopyIsKeptSealedOrSkipped()
    {
        QFETCH(int, mode);
        detectionCase(QStringLiteral("marker-pw-Zq81xT"), marked(), {}, VaultMode(mode), true, QStringLiteral("KeePassXC"));
    }

    void aGeneratedLookingPasswordIsKeptSealedOrStaysOrdinary_data() { aMarkedCopyIsKeptSealedOrSkipped_data(); }
    void aGeneratedLookingPasswordIsKeptSealedOrStaysOrdinary()
    {
        QFETCH(int, mode);
        detectionCase(QStringLiteral("k9#Vf2!qLm8@Zp"), {QStringLiteral("text/plain")}, "detect_passwords = true\n",
                      VaultMode(mode), false, QStringLiteral("Looks like a password"));
    }

    void aCopyThatFitsAUserRuleIsKeptSealedOrStaysOrdinary_data() { aMarkedCopyIsKeptSealedOrSkipped_data(); }
    void aCopyThatFitsAUserRuleIsKeptSealedOrStaysOrdinary()
    {
        QFETCH(int, mode);
        // Six to ten characters, at least three lower-case letters and two digits, ending with a digit.
        detectionCase(QStringLiteral("vqzmpx47"), {QStringLiteral("text/plain")},
                      "password_rules = [\"6-10 lower=3 digits=2 end\"]\n", VaultMode(mode), false,
                      QStringLiteral("Matches your password rule"));
    }

    void anApiKeyTokenOrCardNumberIsKeptSealedOrStaysOrdinary_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("note");
        const std::pair<const char*, VaultMode> modes[] = {{"master password", VaultMode::Master},
                                                           {"use without one", VaultMode::Open},
                                                           {"neither", VaultMode::None}};
        for (const auto& m : modes) {
            QTest::addRow("github token, %s", m.first)
                << int(m.second) << QStringLiteral("ghp_0123456789abcdefghijklmnopqrstuvwxyzAB") << QString();
            QTest::addRow("card number, %s", m.first) << int(m.second) << QStringLiteral("4111 1111 1111 1111") << QString();
        }
    }
    void anApiKeyTokenOrCardNumberIsKeptSealedOrStaysOrdinary()
    {
        QFETCH(int, mode);
        QFETCH(QString, text);
        detectionCase(text, {QStringLiteral("text/plain")}, {}, VaultMode(mode), false,
                      clipitto::secretscan::kindOf(text));
    }

    void aRuleBelowSixCharactersIsNeverUsed()
    {
        // The minimum is part of loading the config: a rule asking for fewer is skipped, so a plain word is not caught.
        writeConfig("password_ask_note = false\npassword_rules = [\"4-10 lower=1\", \"6-10 lower=3 digits=2 end\"]\n");
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QCOMPARE(d.config().passwordRules.size(), 1);
        QCOMPARE(d.config().passwordRules.front().minLength, 6);
        QVERIFY(setUpVault(d, VaultMode::Master));
        emit d.control()->ingestRequested(QStringLiteral("banana"), {QStringLiteral("text/plain")});
        emit d.control()->ingestRequested(QStringLiteral("abc"), {QStringLiteral("text/plain")});
        QCOMPARE(d.repository()->count(), 2);
        QCOMPARE(d.repository()->countSecrets(), 0);
        emit d.control()->ingestRequested(QStringLiteral("zxcvb12"), {QStringLiteral("text/plain")}); // fits the valid rule
        QCOMPARE(d.repository()->countSecrets(), 1);
    }

    void picturesAreNeverKeptAsPasswordCopies()
    {
        for (const VaultMode mode : {VaultMode::Master, VaultMode::Open}) {
            QFile::remove(options().databasePath);
            Daemon d(options(), std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(d, mode));
            QImage picture(40, 40, QImage::Format_RGB32);
            picture.fill(QColor(10, 200, 10));
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            picture.save(&buffer, "PNG");
            // A picture a password manager marked is skipped.
            emit d.control()->ingestImageRequested(png, QStringLiteral("image/png"),
                                                   {QStringLiteral("image/png"), QStringLiteral("x-kde-passwordManagerHint")});
            QTest::qWait(200);
            QCOMPARE(d.repository()->countImages(), 0);
            QCOMPARE(d.repository()->countSecrets(), 0);
            // An ordinary picture is stored as a picture, and moving it to the Passwords group does not mark it.
            emit d.control()->ingestImageRequested(png, QStringLiteral("image/png"), {QStringLiteral("image/png")});
            QTRY_COMPARE(d.repository()->countImages(), 1);
            const qint64 id = d.repository()->allEntries().back().id;
            d.repository()->createGroup(QStringLiteral("Passwords"));
            QSignalSpy markedSpy(d.popup(), &clipitto::ui::PopupWindow::passwordMarked);
            std::unique_ptr<QMenu> menu(d.popup()->createContextMenu(*d.repository()->get(id), d.popup()));
            QAction* toPasswords = menuAction(menu.get(), QStringLiteral("Passwords"));
            QVERIFY(toPasswords);
            toPasswords->trigger();
            QCOMPARE(markedSpy.size(), 0);
            QVERIFY(!d.repository()->get(id)->secret);
            QCOMPARE(d.repository()->countSecrets(), 0);
        }
    }

    // --- The name box after a copy is moved to the Passwords group (a real-use report: it did not open) ---

    // With the settings a new user has (nothing changed), through the Group menu, with the group already there.
    void movingToThePasswordsGroupFromTheMenuOpensTheNameBox()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(d.config().passwordAskNote);
        QVERIFY(setUpVault(d, VaultMode::Master));
        d.repository()->createGroup(QStringLiteral("Passwords"));

        QStringList asked;
        d.popup()->setNamePrompt([&asked](QWidget*, const QString& suggestion) {
            asked << suggestion;
            return std::optional<QString>(QStringLiteral("Bank"));
        });
        const qint64 id = ingestPlain(d, QStringLiteral("my-bank-secret"));
        std::unique_ptr<QMenu> menu(d.popup()->createContextMenu(*d.repository()->get(id), d.popup()));
        QAction* group = menuAction(menu.get(), QStringLiteral("Passwords"));
        QVERIFY(group);
        group->trigger();
        QTRY_COMPARE(asked.size(), 1);
        QVERIFY(d.repository()->get(id)->secret);
        QCOMPARE(d.repository()->get(id)->group, QStringLiteral("Passwords"));
        QTRY_COMPARE(d.repository()->get(id)->note, QStringLiteral("Bank"));
        QTRY_VERIFY(!d.popup()->isVisible());
        QVERIFY(d.status().value(QStringLiteral("password_feature")).toString().contains(QLatin1String("named")));
        QVERIFY(!historyFileHolds("my-bank-secret"));
    }

    // Ctrl+G from the list with the popup open, in the open mode (no master password).
    void ctrlGToThePasswordsGroupOpensTheNameBoxWithoutAMasterPassword()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Open));
        const qint64 id = ingestPlain(d, QStringLiteral("open-mode-secret"));

        QStringList asked;
        d.popup()->setGroupPrompt([](QWidget*, const QString&, const QStringList&) {
            return std::optional<QString>(QStringLiteral("Passwords"));
        });
        d.popup()->setNamePrompt([&asked](QWidget*, const QString& suggestion) {
            asked << suggestion;
            return std::optional<QString>(QStringLiteral("Router"));
        });
        emit d.control()->showRequested(QString());
        QVERIFY(QTest::qWaitForWindowActive(d.popup()));
        auto* view = d.popup()->view();
        view->selectIndex(d.popup()->model()->findEntry(id));
        view->setFocus();
        QTest::keyClick(view, Qt::Key_G, Qt::ControlModifier);
        QTRY_COMPARE(asked.size(), 1);
        QTRY_COMPARE(d.repository()->get(id)->note, QStringLiteral("Router"));
        QVERIFY(d.repository()->get(id)->secret);
        QCOMPARE(d.repository()->get(id)->group, QStringLiteral("Passwords"));
        QVERIFY(d.passwords()->isOpen());
        QCOMPARE(d.passwords()->reveal(id).value_or(QByteArray()), QByteArray("open-mode-secret"));
        QVERIFY(d.popup()->isVisible()); // it was open before: it stays open
        QVERIFY(!historyFileHolds("open-mode-secret"));
    }

    // A sub-group, another spelling, the popup closed, two moves in a row, and the setting read back from the file.
    void theNameBoxOpensForASubGroupAndAgainForTheNextMove()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        QVERIFY(d.applySettings(d.config())); // saved to the file ...
        d.reloadConfig(); // ... and read back: the answer must survive that
        QVERIFY(d.config().passwordAskNote);

        QStringList asked;
        d.popup()->setNamePrompt([&asked](QWidget*, const QString& suggestion) {
            asked << suggestion;
            return std::optional<QString>(QStringLiteral("Name %1").arg(asked.size()));
        });
        QString target = QStringLiteral("passwords/Work");
        d.popup()->setGroupPrompt([&target](QWidget*, const QString&, const QStringList&) {
            return std::optional<QString>(target);
        });
        const auto move = [&](qint64 id) {
            std::unique_ptr<QMenu> menu(d.popup()->createContextMenu(*d.repository()->get(id), d.popup()));
            menuAction(menu.get(), QStringLiteral("New group…"))->trigger();
        };
        const qint64 one = ingestPlain(d, QStringLiteral("sub-group-one"));
        move(one);
        QTRY_COMPARE(asked.size(), 1);
        QVERIFY(d.repository()->get(one)->secret);
        QTRY_COMPARE(d.repository()->get(one)->note, QStringLiteral("Name 1"));
        QTRY_VERIFY(!d.popup()->isVisible());

        target = QStringLiteral("Passwords"); // the group itself, right after
        const qint64 two = ingestPlain(d, QStringLiteral("sub-group-two"));
        move(two);
        QTRY_COMPARE(asked.size(), 2);
        QTRY_COMPARE(d.repository()->get(two)->note, QStringLiteral("Name 2"));
        QCOMPARE(d.repository()->countSecrets(), 2);
    }

    // --- Lock and unlock, through the daemon ---

    void lockNowFromTheCommandLineLocksAndPastingAsksAgain()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        Config quiet = d.config();
        quiet.passwordAskNote = false;
        QVERIFY(d.applySettings(quiet));
        emit d.control()->ingestRequested(QStringLiteral("lock-me-pw"), marked());
        const Entry e = d.repository()->allEntries().back();
        QVERIFY(unlockWith(d, "master"));
        QCOMPARE(d.status().value(QStringLiteral("passwords")).toString().startsWith(QLatin1String("unlocked")), true);
        QCOMPARE(d.passwords()->reveal(e.id).value_or(QByteArray()), QByteArray("lock-me-pw"));

        QSignalSpy lockChanged(d.passwords(), &clipitto::PasswordKeeper::lockChanged);
        emit d.control()->lockPasswordsRequested(); // clipitto --lock, or "Lock now"
        QCOMPARE(d.status().value(QStringLiteral("passwords")).toString(), QStringLiteral("locked"));
        QVERIFY(!d.passwords()->isUnlocked());
        QVERIFY(!d.passwords()->reveal(e.id));
        QCOMPARE(lockChanged.size(), 1);
        QCOMPARE(d.passwords()->secondsLeft(), 0);

        // Pasting it now asks for the master password (a wrong one, then the right one).
        QStringList prompts;
        QString answer = QStringLiteral("wrong");
        d.popup()->setPasswordPrompt([&](QWidget*, const QString& label) {
            prompts << label;
            return std::optional<QString>(answer);
        });
        QStringList notices;
        d.popup()->setNotify([&notices](QWidget*, const QString& m) { notices << m; }); // must not block
        QGuiApplication::clipboard()->clear();
        emit d.control()->showRequested(QString());
        QVERIFY(QTest::qWaitForWindowActive(d.popup()));
        auto* view = d.popup()->view();
        // Recent hides the Passwords group: find the copy through the password search.
        QTest::keyClicks(d.popup()->searchBar(), QStringLiteral("p[] "));
        QTRY_COMPARE(d.popup()->model()->rowCount(), 1);
        view->selectIndex(d.popup()->model()->index(0, 0));
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Return); // a wrong password: stays locked, nothing pasted
        QTRY_COMPARE(prompts.size(), 1);
        QVERIFY(!d.passwords()->isUnlocked());
        QVERIFY(QGuiApplication::clipboard()->text().isEmpty());
        answer = QStringLiteral("master");
        QTRY_COMPARE(notices.size(), 1); // "Wrong master password."
        QVERIFY(notices.at(0).contains(QLatin1String("Wrong")));
        QVERIFY2(d.popup()->isVisible(), "hidden after a wrong password");
        QVERIFY2(d.popup()->model()->rowCount() == 1, qPrintable(QString::number(d.popup()->model()->rowCount())));
        view->selectIndex(d.popup()->model()->index(0, 0));
        view->setFocus();
        QTest::keyClick(view, Qt::Key_Return);
        QTRY_COMPARE(prompts.size(), 2);
        QTRY_VERIFY(d.passwords()->isUnlocked());
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("lock-me-pw"));
        QGuiApplication::clipboard()->clear();
    }

    void changingTheMasterPasswordKeepsPastingAndSearchWorking()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        Config quiet = d.config();
        quiet.passwordAskNote = false;
        QVERIFY(d.applySettings(quiet));
        emit d.control()->ingestRequested(QStringLiteral("survives-change"), marked(QStringLiteral("Bitwarden")));
        const qint64 id = d.repository()->allEntries().back().id;

        std::optional<bool> changed;
        d.passwords()->changeMasterPassword("wrong", "Second-pw-1x", [&](bool ok) { changed = ok; });
        QTRY_VERIFY(changed.has_value());
        QVERIFY(!*changed); // the old one must be right
        changed.reset();
        d.passwords()->changeMasterPassword("master", "Second-pw-1x", [&](bool ok) { changed = ok; });
        QTRY_VERIFY(changed.has_value());
        QVERIFY(*changed);
        QVERIFY(!d.passwords()->isUnlocked()); // a new key: locked again
        QVERIFY(!unlockWith(d, "master"));
        QVERIFY(unlockWith(d, "Second-pw-1x"));
        QCOMPARE(d.passwords()->reveal(id).value_or(QByteArray()), QByteArray("survives-change"));
        QCOMPARE(d.passwords()->search(QStringLiteral("survives")).size(), size_t(1));
        // The same password copied again is the same copy, not a second one (the match key did not change).
        emit d.control()->ingestRequested(QStringLiteral("survives-change"), marked(QStringLiteral("Bitwarden")));
        QCOMPARE(d.repository()->countSecrets(), 1);
        QVERIFY(!historyFileHolds("survives-change"));
    }

    void removingTheMasterPasswordDeletesPasswordCopiesAndKeepsTheRest()
    {
        DaemonOptions o = options();
        auto keys = std::make_shared<FakeKeyStore>();
        o.matchKeyStore = keys;
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        Config quiet = d.config();
        quiet.passwordAskNote = false;
        QVERIFY(d.applySettings(quiet));
        emit d.control()->ingestRequested(QStringLiteral("to-be-removed"), marked());
        emit d.control()->ingestRequested(QStringLiteral("an ordinary copy"), {QStringLiteral("text/plain")});
        QCOMPARE(d.repository()->countSecrets(), 1);
        QVERIFY(d.control()->passwordsWanted());

        d.passwords()->reset(); // Settings > Passwords > Remove
        QCOMPARE(d.repository()->countSecrets(), 0);
        QCOMPARE(d.repository()->count(), 1);
        QVERIFY(!d.passwords()->isSetUp());
        QVERIFY(!d.control()->passwordsWanted()); // helpers are told to stop reading them
        QVERIFY(d.status().value(QStringLiteral("passwords")).toString().startsWith(QLatin1String("off")));
        QVERIFY(!keys->stored); // the match key went with it
        // A marked copy is skipped again, and a new master password can be set.
        emit d.control()->ingestRequested(QStringLiteral("after-removal"), marked());
        QCOMPARE(d.repository()->count(), 1);
        QVERIFY(!historyFileHolds("to-be-removed"));
        QVERIFY(!historyFileHolds("after-removal"));
        QVERIFY(setUpVault(d, VaultMode::Master));
        emit d.control()->ingestRequested(QStringLiteral("after-removal"), marked());
        QCOMPARE(d.repository()->countSecrets(), 1);
    }

    // --- Never in clear ---

    void thePasswordIsNeverInTheHistoryFile()
    {
        const QByteArray secrets[] = {"file-pw-marked", "file-pw-by-hand", "file-pw-moved",
                                      "ghp_0123456789abcdefghijklmnopqrstuvwxyzAB"};
        {
            DaemonOptions o = options();
            o.matchKeyStore = std::make_shared<FakeKeyStore>();
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(d, VaultMode::Master));
            Config c = d.config();
            c.passwordAskNote = false;
            QVERIFY(d.applySettings(c));
            emit d.control()->ingestRequested(QString::fromLatin1(secrets[0]), marked());
            emit d.control()->ingestRequested(QString::fromLatin1(secrets[3]), {QStringLiteral("text/plain")});
            const qint64 byHand = ingestPlain(d, QString::fromLatin1(secrets[1]));
            QVERIFY(d.passwords()->markAsPassword(byHand));
            d.repository()->createGroup(QStringLiteral("Passwords"));
            const qint64 moved = ingestPlain(d, QString::fromLatin1(secrets[2]));
            std::unique_ptr<QMenu> menu(d.popup()->createContextMenu(*d.repository()->get(moved), d.popup()));
            menuAction(menu.get(), QStringLiteral("Passwords"))->trigger();
            QCOMPARE(d.repository()->countSecrets(), 4);
            for (const QByteArray& s : secrets) {
                QVERIFY2(!historyFileHolds(s), s.constData()); // while the database is open (WAL included)
            }
        }
        for (const QByteArray& s : secrets) {
            QVERIFY2(!historyFileHolds(s), s.constData()); // and after it is closed and checkpointed
        }
    }

    void theDebugLogNeverHoldsAPassword()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        o.debugLogPath = m_dir.filePath(QStringLiteral("logs/debug.log"));
        writeConfig("debug_log = true\ndetect_passwords = true\n");
        const QStringList secrets = {QStringLiteral("log-pw-marked"), QStringLiteral("k9#Vf2!qLm8@Zp"),
                                     QStringLiteral("ghp_0123456789abcdefghijklmnopqrstuvwxyzAB"),
                                     QStringLiteral("log-pw-moved"), QStringLiteral("Tr0ub4dor&3x-near-miss")};
        {
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(d, VaultMode::Master));
            d.popup()->setNotify([](QWidget*, const QString&) {});
            d.popup()->setNamePrompt([](QWidget*, const QString&) { return std::optional<QString>(QStringLiteral("Named")); });
            emit d.control()->ingestRequested(secrets[0], marked()); // marked, with the name box
            QTRY_COMPARE(d.repository()->allEntries().back().note, QStringLiteral("Named"));
            QTRY_VERIFY(!d.popup()->isVisible());
            emit d.control()->ingestRequested(secrets[1], {QStringLiteral("text/plain")}); // detected
            emit d.control()->ingestRequested(secrets[2], {QStringLiteral("text/plain")});
            emit d.control()->ingestRequested(secrets[4], {QStringLiteral("text/plain")}); // a near miss is logged by class only
            // The detected copies each open a name box; let those finish (one box at a time, by design).
            QTRY_COMPARE(d.status().value(QStringLiteral("password_feature")).toString().count(QLatin1String("named")), 1);
            QTest::qWait(300);
            QTRY_VERIFY(!d.popup()->isVisible());
            const qint64 moved = ingestPlain(d, secrets[3]);
            d.repository()->createGroup(QStringLiteral("Passwords"));
            std::unique_ptr<QMenu> menu(d.popup()->createContextMenu(*d.repository()->get(moved), d.popup()));
            menuAction(menu.get(), QStringLiteral("Passwords"))->trigger();
            QTRY_COMPARE(d.repository()->get(moved)->note, QStringLiteral("Named"));
            // Pasting one back, locking, a wrong unlock.
            QVERIFY(unlockWith(d, "master"));
            emit d.popup()->entryActivated(*d.repository()->get(moved));
            emit d.control()->lockPasswordsRequested();
            QVERIFY(!unlockWith(d, "not-the-master"));
            QGuiApplication::clipboard()->clear();
        }
        QFile file(o.debugLogPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(file.readAll());
        file.close();
        QVERIFY(!log.isEmpty());
        QVERIFY(log.contains(QLatin1String("name box"))); // the flow was logged ...
        for (const QString& s : secrets + QStringList{QStringLiteral("not-the-master")}) {
            QVERIFY2(!log.contains(s), "a password reached debug.log");
            QVERIFY2(!log.contains(QString::fromLatin1(s.toUtf8().toBase64())), "a password reached debug.log (base64)");
        }
    }

    void theMarkersOnAPastedPasswordNeverHoldIt()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        Config c = d.config();
        c.passwordAskNote = false;
        c.passwordClearSeconds = 0; // stays on the clipboard so the test can look at it
        QVERIFY(d.applySettings(c));
        emit d.control()->ingestRequested(QStringLiteral("marker-check-pw"), marked());
        const Entry e = d.repository()->allEntries().back();
        QVERIFY(unlockWith(d, "master"));
        emit d.popup()->entryActivated(e);
        const QMimeData* data = QGuiApplication::clipboard()->mimeData();
        QVERIFY(data);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("marker-check-pw"));
        QVERIFY(clipitto::ClipFilter::isSensitive(data->formats()));
        int markers = 0;
        for (const QString& format : data->formats()) {
            if (clipitto::ClipFilter::isSensitive({format})) {
                ++markers;
                const QByteArray bytes = data->data(format);
                QVERIFY2(!bytes.contains("marker-check-pw"), qPrintable(format));
                QVERIFY2(bytes.size() < 16, qPrintable(format)); // a flag, not a copy of the text
            }
        }
        QVERIFY(markers >= 1);
        QGuiApplication::clipboard()->clear();
    }

    // --- Backup of password copies captured by the daemon, restored into another one ---

    void passwordsCapturedByTheDaemonSurviveBackupAndRestore_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("encrypted with the master password") << int(VaultMode::Master);
        QTest::newRow("in clear text, without one") << int(VaultMode::Open);
    }
    void passwordsCapturedByTheDaemonSurviveBackupAndRestore()
    {
        QFETCH(int, mode);
        const VaultMode vault = VaultMode(mode);
        const QString file = m_dir.filePath(QStringLiteral("backup.json"));
        QFile::remove(file);
        {
            DaemonOptions o = options();
            o.matchKeyStore = std::make_shared<FakeKeyStore>();
            Daemon a(o, std::make_unique<FakeControl>());
            QVERIFY(a.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(a, vault));
            Config c = a.config();
            c.passwordAskNote = false;
            QVERIFY(a.applySettings(c));
            emit a.control()->ingestRequested(QStringLiteral("backup-pw-one"), marked(QStringLiteral("KeePassXC")));
            emit a.control()->ingestRequested(QStringLiteral("backup-pw-two"), marked(QStringLiteral("Safari")));
            emit a.control()->ingestRequested(QStringLiteral("a plain copy"), {QStringLiteral("text/plain")});
            QCOMPARE(a.repository()->countSecrets(), 2);
            if (vault == VaultMode::Master) {
                QVERIFY(unlockWith(a, "master"));
            }
            QString error;
            QVERIFY2(clipitto::backup::exportTo(file, *a.repository(), a.config(), a.passwords(), {true, false}, "master",
                                                &error, vault == VaultMode::Master ? clipitto::backup::PasswordExport::Encrypted
                                                                                    : clipitto::backup::PasswordExport::Clear),
                     qPrintable(error));
        }
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray contents = f.readAll();
        f.close();
        const auto info = clipitto::backup::inspect(file);
        QCOMPARE(info.passwords, 2);
        QCOMPARE(info.passwordsClear, vault == VaultMode::Open);
        if (vault == VaultMode::Master) {
            QVERIFY(!contents.contains("backup-pw-one")); // never in clear, nor as plain base64
            QVERIFY(!contents.contains(QByteArray("backup-pw-one").toBase64()));
        }

        // A second computer with its own master password.
        DaemonOptions o2;
        o2.configPath = m_dir.filePath(QStringLiteral("config2.toml"));
        o2.databasePath = m_dir.filePath(QStringLiteral("data2/history.db"));
        o2.syntheticPaste = [](PasteKeys) { return false; };
        o2.matchKeyStore = std::make_shared<FakeKeyStore>();
        QFile::remove(o2.databasePath);
        Daemon b(o2, std::make_unique<FakeControl>());
        QVERIFY(b.start() == Daemon::StartResult::Ok);
        b.passwords()->setKdfCost({64, 1});
        std::optional<bool> set;
        b.passwords()->setMasterPassword("another master", [&set](bool ok) { set = ok; });
        QTRY_VERIFY(set.has_value());
        QVERIFY(*set);
        QVERIFY(unlockWith(b, "another master"));
        QString error;
        const auto report = clipitto::backup::importFrom(file, {true, false}, *b.repository(), b.passwords(), "master", &error);
        QVERIFY2(report, qPrintable(error));
        QCOMPARE(report->passwordsAdded, 2);
        QCOMPARE(report->passwordsSkipped, 0);
        QCOMPARE(b.repository()->countSecrets(), 2);
        std::set<QByteArray> restored;
        for (const Entry& e : b.repository()->allEntries()) {
            if (e.secret) {
                restored.insert(b.passwords()->reveal(e.id).value_or(QByteArray()));
                QCOMPARE(e.group, QStringLiteral("Passwords"));
            }
        }
        QCOMPARE(restored, (std::set<QByteArray>{"backup-pw-one", "backup-pw-two"}));
        // The restored copies are in the second computer's own file only as sealed copies.
        QFile second(o2.databasePath);
        QVERIFY(second.open(QIODevice::ReadOnly));
        QVERIFY(!second.readAll().contains("backup-pw-one"));
        second.close();
    }

    // --- Grouping through the popup ---

    // --- lock_on_screen_lock: a screen lock or a sleep locks the vault, through a fake watcher ---

    void aScreenLockLocksAnUnlockedVaultAndWipesTheKey_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::newRow("screen lock") << QStringLiteral("screen lock");
        QTest::newRow("sleep") << QStringLiteral("sleep");
        QTest::newRow("disconnect") << QStringLiteral("disconnect");
    }

    void aScreenLockLocksAnUnlockedVaultAndWipesTheKey()
    {
        QFETCH(QString, reason);
        ScreenLockEnv e;
        startScreenLockEnv(e, VaultMode::Master);
        Daemon& d = *e.d;
        QVERIFY(e.watcher->isWatching()); // on by default
        QVERIFY(d.config().lockOnScreenLock);
        QCOMPARE(lockLine(d), QStringLiteral("on (watching)"));
        QVERIFY(d.passwords()->isUnlocked());
        QVERIFY(d.passwords()->secondsLeft() > 0);
        QCOMPARE(d.passwords()->reveal(e.secret.id).value_or(QByteArray()), QByteArray("screen-lock-pw"));

        QSignalSpy lockChanged(d.passwords(), &clipitto::PasswordKeeper::lockChanged);
        e.watcher->simulateLock(reason);
        QVERIFY(!d.passwords()->isUnlocked());
        QVERIFY(!d.passwords()->vault().isUnlocked()); // the key in memory is gone
        QVERIFY(!d.passwords()->reveal(e.secret.id));
        QCOMPARE(d.passwords()->secondsLeft(), 0); // the unlock timer stopped with it
        QCOMPARE(lockChanged.size(), 1);
        QCOMPARE(d.status().value(QStringLiteral("passwords")).toString(), QStringLiteral("locked"));
        QCOMPARE(lockLine(d), QStringLiteral("on (watching)"));
    }

    void aScreenLockIsIgnoredWhenTheSettingIsOffAndWorksAgainWhenItIsOn()
    {
        ScreenLockEnv e;
        startScreenLockEnv(e, VaultMode::Master);
        Daemon& d = *e.d;
        Config off = d.config();
        off.lockOnScreenLock = false;
        QVERIFY(d.applySettings(off));
        QVERIFY(!e.watcher->isWatching()); // not even listening
        QCOMPARE(lockLine(d), QStringLiteral("off"));
        e.watcher->simulateLock();
        emit e.watcher->locked(QStringLiteral("screen lock")); // an event that still arrives is ignored too
        QVERIFY(d.passwords()->isUnlocked());
        QVERIFY(d.passwords()->reveal(e.secret.id).has_value());

        Config on = d.config();
        on.lockOnScreenLock = true;
        QVERIFY(d.applySettings(on));
        QVERIFY(e.watcher->isWatching());
        QCOMPARE(lockLine(d), QStringLiteral("on (watching)"));
        e.watcher->simulateLock();
        QVERIFY(!d.passwords()->isUnlocked());
    }

    void aScreenLockDoesNothingWhenLockedOrWithoutAMasterPassword()
    {
        { // already locked: no second lock, no change signal
            ScreenLockEnv e;
            startScreenLockEnv(e, VaultMode::Master, /*unlock=*/false);
            QVERIFY(!e.d->passwords()->isUnlocked());
            QSignalSpy lockChanged(e.d->passwords(), &clipitto::PasswordKeeper::lockChanged);
            e.watcher->simulateLock();
            QCOMPARE(lockChanged.size(), 0);
            QVERIFY(!e.d->passwords()->isUnlocked());
            QVERIFY(unlockWith(*e.d, "master")); // and unlocking still works afterwards
            QVERIFY(e.d->passwords()->isUnlocked());
        }
        { // open mode (no master password) never locks
            init(); // a fresh history file: the one above has a master password
            ScreenLockEnv e;
            startScreenLockEnv(e, VaultMode::Open);
            QVERIFY(e.d->passwords()->isOpen());
            QVERIFY(e.d->passwords()->isUnlocked());
            e.watcher->simulateLock();
            QVERIFY(e.d->passwords()->isUnlocked());
            QVERIFY(e.d->passwords()->reveal(e.secret.id).has_value());
        }
        { // no vault at all
            init();
            ScreenLockEnv e;
            startScreenLockEnv(e, VaultMode::None);
            e.watcher->simulateLock();
            QVERIFY(!e.d->passwords()->isSetUp());
        }
    }

    void theUnlockTimerStillRunsAfterAScreenLockAndAnUnlock()
    {
        ScreenLockEnv e;
        startScreenLockEnv(e, VaultMode::Master);
        Daemon& d = *e.d;
        e.watcher->simulateLock();
        QCOMPARE(d.passwords()->secondsLeft(), 0);
        QVERIFY(unlockWith(d, "master"));
        // The timer is back, with the setting's length (5 minutes by default).
        QVERIFY(d.passwords()->secondsLeft() > 290 && d.passwords()->secondsLeft() <= 300);
        QVERIFY(d.status().value(QStringLiteral("passwords")).toString().startsWith(QLatin1String("unlocked")));
        d.passwords()->lock(); // Lock now / --lock keep working
        QVERIFY(!d.passwords()->isUnlocked());
    }

    void theStatusLineSaysWhenThisSystemGivesNoLockEvents()
    {
        { // a watcher that cannot work here
            DaemonOptions o = options();
            o.matchKeyStore = std::make_shared<FakeKeyStore>();
            auto unsupported = std::make_shared<clipitto::FakeScreenLockWatcher>(/*supported=*/false);
            o.screenLockWatcher = unsupported;
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(d, VaultMode::Master));
            QVERIFY(unsupported->startCalls >= 1);
            QCOMPARE(lockLine(d), QStringLiteral("on (unsupported here)"));
            Config off = d.config();
            off.lockOnScreenLock = false;
            QVERIFY(d.applySettings(off));
            QCOMPARE(lockLine(d), QStringLiteral("off"));
        }
        { // no watcher given at all
            init();
            DaemonOptions o = options();
            o.matchKeyStore = std::make_shared<FakeKeyStore>();
            Daemon d(o, std::make_unique<FakeControl>());
            QVERIFY(d.start() == Daemon::StartResult::Ok);
            QVERIFY(setUpVault(d, VaultMode::Master));
            QCOMPARE(lockLine(d), QStringLiteral("on (unsupported here)"));
        }
    }

    void theSettingIsReadFromTheConfigFileAndTheWatcherFollowsIt()
    {
        writeConfig("lock_on_screen_lock = false\n");
        ScreenLockEnv e;
        startScreenLockEnv(e, VaultMode::Master);
        QVERIFY(!e.d->config().lockOnScreenLock);
        QVERIFY(!e.watcher->isWatching());
        QCOMPARE(lockLine(*e.d), QStringLiteral("off"));
        writeConfig("lock_on_screen_lock = true\n");
        e.d->reloadConfig();
        QVERIFY(e.watcher->isWatching());
        e.watcher->simulateLock();
        QVERIFY(!e.d->passwords()->isUnlocked());
    }

    void ctrlGAndTheGroupMenuFileACopyAndNestedGroupsAreMade()
    {
        Daemon d(options(), std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        const qint64 one = ingestPlain(d, QStringLiteral("kubectl get nodes"));
        const qint64 two = ingestPlain(d, QStringLiteral("ssh prod"));
        d.repository()->setFavorite(two, true);
        QString answer = QStringLiteral("Work/Servers/Prod");
        d.popup()->setGroupPrompt([&answer](QWidget*, const QString&, const QStringList&) {
            return std::optional<QString>(answer);
        });
        emit d.control()->showRequested(QString());
        QVERIFY(QTest::qWaitForWindowActive(d.popup()));
        auto* view = d.popup()->view();
        view->selectIndex(d.popup()->model()->findEntry(one));
        view->setFocus();
        QTest::keyClick(view, Qt::Key_G, Qt::ControlModifier); // Ctrl+G: a nested path creates its parents
        QCOMPARE(d.repository()->get(one)->group, QStringLiteral("Work/Servers/Prod"));
        for (const char* g : {"Work", "Work/Servers", "Work/Servers/Prod"}) {
            QVERIFY2(d.repository()->groups().contains(QString::fromLatin1(g)), g);
        }
        // The Group menu: an existing group, and out again; a star goes along.
        std::unique_ptr<QMenu> menu(d.popup()->createContextMenu(*d.repository()->get(two), d.popup()));
        QAction* toServers = menuAction(menu.get(), clipitto::groups::display(QStringLiteral("Work/Servers")));
        QVERIFY(toServers);
        toServers->trigger();
        QCOMPARE(d.repository()->get(two)->group, QStringLiteral("Work/Servers"));
        QVERIFY(d.repository()->get(two)->favorite);
        menu.reset(d.popup()->createContextMenu(*d.repository()->get(two), d.popup()));
        QAction* removeFromGroup = menuAction(menu.get(), QStringLiteral("Remove from group"));
        QVERIFY(removeFromGroup);
        removeFromGroup->trigger();
        QVERIFY(d.repository()->get(two)->group.isEmpty());
        QVERIFY(d.repository()->get(two)->favorite);
        // The tree in the popup shows the nesting.
        const QModelIndex prod = d.popup()->model()->findGroup(QStringLiteral("Work/Servers/Prod"));
        QVERIFY(prod.isValid());
        QCOMPARE(prod.parent(), d.popup()->model()->findGroup(QStringLiteral("Work/Servers")));
        QCOMPARE(d.repository()->count(), 2);
    }

    void deletingTheGroupThatHoldsPasswordsKeepsThemAsPasswords()
    {
        DaemonOptions o = options();
        o.matchKeyStore = std::make_shared<FakeKeyStore>();
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        QVERIFY(setUpVault(d, VaultMode::Master));
        Config c = d.config();
        c.passwordAskNote = false;
        QVERIFY(d.applySettings(c));
        emit d.control()->ingestRequested(QStringLiteral("grouped-pw"), marked());
        const qint64 pw = d.repository()->allEntries().back().id;
        QCOMPARE(d.repository()->get(pw)->group, QStringLiteral("Passwords"));
        const qint64 plain = ingestPlain(d, QStringLiteral("plain in work"));
        d.repository()->setGroup(plain, QStringLiteral("Work"));
        d.popup()->setConfirmPrompt([](QWidget*, const QString&) { return true; });

        emit d.control()->showRequested(QString());
        QVERIFY(QTest::qWaitForWindowActive(d.popup()));
        auto* view = d.popup()->view();
        QTest::keyClick(d.popup()->searchBar(), Qt::Key_Right); // into the groups
        view->selectIndex(d.popup()->model()->findGroup(QStringLiteral("Passwords")));
        QTest::keyClick(view, Qt::Key_Delete);
        QVERIFY(!d.repository()->groups().contains(QStringLiteral("Passwords")));
        QVERIFY(d.repository()->groups().contains(QStringLiteral("Work")));
        // The copy stays a sealed password copy, ungrouped, and still opens.
        QVERIFY(d.repository()->get(pw));
        QVERIFY(d.repository()->get(pw)->secret);
        QVERIFY(d.repository()->get(pw)->group.isEmpty());
        QVERIFY(unlockWith(d, "master"));
        QCOMPARE(d.passwords()->reveal(pw).value_or(QByteArray()), QByteArray("grouped-pw"));
        // The next captured password makes the group again.
        emit d.control()->ingestRequested(QStringLiteral("next-pw"), marked());
        QCOMPARE(d.repository()->allEntries().back().group, QStringLiteral("Passwords"));
        QVERIFY(d.repository()->groups().contains(QStringLiteral("Passwords")));
    }
};

QTEST_MAIN(TestDaemon)
#include "tst_daemon.moc"
