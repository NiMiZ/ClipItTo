#include "FakeKeyStore.h"
#include "SelfTest.h"
#include "SelfTestChecks.h"
#include "StallWatchdog.h"
#include "db/Migrator.h"
#include "db/Sqlite.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <thread>

using namespace clipitto;
using namespace clipitto::selftest;

namespace {

// A string that must never appear in a report: it stands for the clipboard, a key, a window title, a folder name.
const QString kSecret = QStringLiteral("SECRET-MARKER-8f3a");

Env baseEnv()
{
    Env env;
    env.version = QStringLiteral("9.9.9");
    env.platform = QStringLiteral("TestOS 1");
    env.session.display = SessionEnv::Display::X11;
    env.configPath = QStringLiteral("/cfg/config.toml");
    env.databasePath = QStringLiteral("/data/history.db");
    env.logPath = QStringLiteral("/data/debug.log");
    env.status = {{QStringLiteral("backend"), QStringLiteral("qt-clipboard")},
                  {QStringLiteral("backend_degraded"), false},
                  {QStringLiteral("backend_note"), QStringLiteral("auto")},
                  {QStringLiteral("shortcut"), QStringLiteral("Ctrl+Alt+V (GNOME)")}};
    return env;
}

Report runAll(const Env& env, double scale = 1.0)
{
    Runner runner(buildChecks(env), QStringLiteral("header"));
    runner.setTimeScale(scale);
    QSignalSpy done(&runner, &Runner::finished);
    runner.start();
    done.wait(60000);
    return done.isEmpty() ? Report{} : qvariant_cast<Report>(done.takeFirst().at(0));
}

} // namespace

class TestSelfTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<Report>(); qRegisterMetaType<Result>(); }

    // --- the runner -------------------------------------------------------------------------------

    void runnerRunsChecksInOrderAndKeepsTheirIds()
    {
        QList<Check> checks;
        for (const char* id : {"a", "b", "c"}) {
            Check c;
            c.id = QLatin1String(id);
            c.title = QString::fromLatin1(id).toUpper();
            c.run = [] { return Result::pass(QStringLiteral("fine")); };
            checks.push_back(c);
        }
        Check async;
        async.id = QStringLiteral("d");
        async.title = QStringLiteral("D");
        async.start = [](Done done) { QTimer::singleShot(20, [done] { done(Result::warn(QStringLiteral("hm"), QStringLiteral("do x"))); }); };
        checks.push_back(async);
        Runner runner(checks, QStringLiteral("hdr"));
        QSignalSpy each(&runner, &Runner::checkFinished);
        QSignalSpy done(&runner, &Runner::finished);
        runner.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 20000);
        QCOMPARE(each.size(), 4);
        const Report report = qvariant_cast<Report>(done.first().at(0));
        QCOMPARE(report.results.size(), 4);
        QCOMPARE(report.results.at(0).id, QStringLiteral("a"));
        QCOMPARE(report.results.at(3).title, QStringLiteral("D"));
        QCOMPARE(report.results.at(3).outcome, Outcome::Warn);
        QVERIFY(!report.failed());
        QVERIFY(!runner.isRunning());
    }

    void aBlockedCheckBecomesAFailAndTheRunGoesOn()
    {
        Check hang;
        hang.id = QStringLiteral("hang");
        hang.title = QStringLiteral("Hanging");
        hang.timeoutMs = 300;
        hang.run = [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // a keyring that never answers
            return Result::pass(QStringLiteral("too late"));
        };
        Check never;
        never.id = QStringLiteral("never");
        never.title = QStringLiteral("Never calls done");
        never.timeoutMs = 300;
        never.start = [](Done) {};
        Check after;
        after.id = QStringLiteral("after");
        after.title = QStringLiteral("After");
        after.run = [] { return Result::pass(QStringLiteral("ok")); };
        Runner runner({hang, never, after}, QString());
        QSignalSpy done(&runner, &Runner::finished);
        runner.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        const Report report = qvariant_cast<Report>(done.first().at(0));
        QCOMPARE(report.results.size(), 3);
        QCOMPARE(report.results.at(0).outcome, Outcome::Fail);
        QVERIFY2(report.results.at(0).detail.startsWith(QLatin1String("did not answer in ")), qPrintable(report.results.at(0).detail));
        QVERIFY(report.results.at(0).detail.endsWith(QLatin1String(" s")));
        QVERIFY(!report.results.at(0).advice.isEmpty());
        QCOMPARE(report.results.at(1).outcome, Outcome::Fail);
        QCOMPARE(report.results.at(2).outcome, Outcome::Pass);
        QVERIFY(report.failed());
        // The late answer of the first check must not change anything or add a result.
        QTest::qWait(1800);
        QCOMPARE(done.size(), 1);
    }

    void aLateAnswerIsDropped()
    {
        Check slow;
        slow.id = QStringLiteral("slow");
        slow.title = QStringLiteral("Slow");
        slow.timeoutMs = 200;
        Done saved;
        slow.start = [&saved](Done done) { saved = done; };
        Runner runner({slow}, QString());
        QSignalSpy done(&runner, &Runner::finished);
        runner.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 20000);
        saved(Result::pass(QStringLiteral("late")));
        QTest::qWait(100);
        QCOMPARE(done.size(), 1);
        const Report report = qvariant_cast<Report>(done.first().at(0));
        QCOMPARE(report.results.at(0).outcome, Outcome::Fail);
    }

    void aRunnerDeletedWhileACheckIsOpenIsSafe()
    {
        Done saved;
        {
            Check c;
            c.id = QStringLiteral("x");
            c.title = QStringLiteral("X");
            c.start = [&saved](Done done) { saved = done; };
            Runner runner({c}, QString());
            runner.start();
        }
        saved(Result::pass(QStringLiteral("after the runner is gone"))); // must not crash
        QTest::qWait(20);
    }

    // --- the report -------------------------------------------------------------------------------

    void reportTextHasWordsAdviceSummaryAndTheRule()
    {
        Report report;
        report.header = QStringLiteral("hdr line");
        report.results = {{QStringLiteral("a"), QStringLiteral("Alpha"), Outcome::Pass, QStringLiteral("all good"), {}},
                          {QStringLiteral("b"), QStringLiteral("Beta"), Outcome::Warn, QStringLiteral("so-so"), QStringLiteral("do this")},
                          {QStringLiteral("c"), QStringLiteral("Gamma"), Outcome::Fail, QStringLiteral("broken"), QStringLiteral("fix that")},
                          {QStringLiteral("d"), QStringLiteral("Delta"), Outcome::Skipped, QStringLiteral("n/a"), {}}};
        const QString text = report.text();
        QVERIFY(text.contains(QLatin1String("[PASS] Alpha: all good")));
        QVERIFY(text.contains(QLatin1String("[WARN] Beta: so-so")));
        QVERIFY(text.contains(QLatin1String("To do: do this")));
        QVERIFY(text.contains(QLatin1String("[FAIL] Gamma: broken")));
        QVERIFY(text.contains(QLatin1String("[SKIPPED] Delta: n/a")));
        QVERIFY(text.contains(QLatin1String("hdr line")));
        QVERIFY(text.contains(QLatin1String("1 PASS, 1 WARN, 1 FAIL, 1 SKIPPED")));
        QVERIFY(text.contains(QLatin1String("never contains what you copied")));
        QVERIFY(report.failed());
        QCOMPARE(report.toMap().value(QStringLiteral("fail")).toInt(), 1);
        QCOMPARE(report.toMap().value(QStringLiteral("report")).toString(), text);
        QVERIFY(report.find(QStringLiteral("c")) != nullptr);
        QVERIFY(report.find(QStringLiteral("zzz")) == nullptr);
    }

    void aWholeReportNeverCarriesSecrets()
    {
        // Everything a secret could come through is given the marker: a keyring, the sync folder and its status line, the
        // clipboard test, the paste target's second id, the user's rule patterns, the last name box outcome stays a fixed word.
        Env env = baseEnv();
        auto history = std::make_shared<FakeKeyStore>();
        history->stored = kSecret.toUtf8();
        auto match = std::make_shared<FakeKeyStore>();
        match->stored = kSecret.toUtf8();
        auto syncKeys = std::make_shared<FakeKeyStore>();
        syncKeys->stored = kSecret.toUtf8();
        env.historyKeys = history;
        env.matchKeys = match;
        env.syncKeys = syncKeys;
        env.historyKeyNeeded = env.matchKeyNeeded = env.syncKeyNeeded = true;
        env.config.syncEnabled = true;
        env.config.syncFolder = QStringLiteral("/home/%1/sync").arg(kSecret);
        env.status[QStringLiteral("sync")] = QStringLiteral("on, 1 other device(s); waiting: cannot read %1/x.sync").arg(env.config.syncFolder);
        env.passwordsAvailable = env.passwordsSetUp = env.canMatch = true;
        env.passwordState = QStringLiteral("locked");
        env.lastNameBox = QStringLiteral("named at 10:00:00");
        env.pasteTarget = [] { return PasteTarget::known({QStringLiteral("firefox"), QStringLiteral("Navigator")}); };
        env.captureProbe = [](std::function<void(CaptureOutcome)> done) { done(CaptureOutcome::Seen); };
        env.syncFacts = [] { return SyncFacts{true, true, true, true}; };
        env.database = [] {
            DbFacts f;
            f.opened = f.writable = true;
            f.quickCheck = QStringLiteral("ok");
            f.schema = f.expectedSchema = 11;
            f.freeBytes = 10LL << 30;
            f.fileBytes = 1 << 20;
            return f;
        };
        env.configFile = [] { return ConfigFacts{true, {}}; };
        env.logFile = [] { return LogFacts{true, {}}; };
        const Report report = runAll(env);
        QVERIFY(report.results.size() >= 18);
        QVERIFY2(!report.text().contains(kSecret), qPrintable(report.text()));
        for (const Result& r : report.results) {
            QVERIFY2(!r.detail.contains(kSecret) && !r.advice.contains(kSecret) && !r.title.contains(kSecret), qPrintable(r.id));
        }
        // The sync status line was kept but without the user's folder.
        QVERIFY(report.find(QStringLiteral("sync"))->detail.contains(QLatin1String("<sync folder>")));
        // The keyring was read for real (needed), and the key is not in the text.
        QCOMPARE(report.find(QStringLiteral("keyring_history"))->outcome, Outcome::Pass);
        QCOMPARE(report.find(QStringLiteral("keyring_passwords"))->outcome, Outcome::Pass);
        QCOMPARE(report.find(QStringLiteral("keyring_sync"))->outcome, Outcome::Pass);
    }

    void everyCheckHasAnIdAndATitleInTheListOrder()
    {
        const QList<Check> checks = buildChecks(baseEnv());
        QStringList ids;
        for (const Check& c : checks) {
            QVERIFY(!c.id.isEmpty() && !c.title.isEmpty());
            QVERIFY(bool(c.run) != bool(c.start));
            ids << c.id;
        }
        QVERIFY(ids.removeDuplicates() == 0);
        for (const char* id : {"daemon", "capture_backend", "hotkey", "paste", "keyring_history", "keyring_passwords", "keyring_sync",
                               "passwords", "paste_target", "gnome_extension", "screen_lock", "sync", "forget_panic", "hello"}) {
            QVERIFY2(ids.contains(QLatin1String(id)), id);
        }
    }

    // --- single checks, pass / warn / fail / skipped ----------------------------------------------

    void daemonCheck_()
    {
        Env env = baseEnv();
        QCOMPARE(daemonCheck(env).outcome, Outcome::Pass);
        QVERIFY(daemonCheck(env).detail.contains(QLatin1String("9.9.9")));
        QVERIFY(daemonCheck(env).detail.contains(QLatin1String("X11")));
        env.session.display = SessionEnv::Display::Unknown;
        QCOMPARE(daemonCheck(env).outcome, Outcome::Warn);
    }

    void captureBackend()
    {
        Env env = baseEnv();
        QCOMPARE(captureBackendCheck(env).outcome, Outcome::Pass);
        env.status[QStringLiteral("backend_degraded")] = true;
        env.status[QStringLiteral("backend_note")] = QStringLiteral("no data-control");
        env.session = {SessionEnv::Display::Wayland, true};
        const Result gnome = captureBackendCheck(env);
        QCOMPARE(gnome.outcome, Outcome::Warn);
        QVERIFY(gnome.detail.contains(QLatin1String("no data-control")));
        QVERIFY(gnome.advice.contains(QLatin1String("GNOME Shell extension")));
        env.session = {SessionEnv::Display::Wayland, false};
        QVERIFY(captureBackendCheck(env).advice.contains(QLatin1String("wl-clipboard")));
        env.status.remove(QStringLiteral("backend"));
        QCOMPARE(captureBackendCheck(env).outcome, Outcome::Fail);
    }

    void captureCopy()
    {
        QCOMPARE(captureCopyResult(CaptureOutcome::Seen).outcome, Outcome::Pass);
        QCOMPARE(captureCopyResult(CaptureOutcome::NotSeen).outcome, Outcome::Fail);
        QCOMPARE(captureCopyResult(CaptureOutcome::CannotWrite).outcome, Outcome::Warn);
        QCOMPARE(captureCopyResult(CaptureOutcome::Paused).outcome, Outcome::Skipped);
        QCOMPARE(captureCopyResult(CaptureOutcome::NoHistory).outcome, Outcome::Skipped);
    }

    void hotkey()
    {
        Env env = baseEnv();
        QCOMPARE(hotkeyCheck(env).outcome, Outcome::Pass);
        env.status[QStringLiteral("shortcut")] = QStringLiteral("not set automatically: bind \"clipitto --toggle\"");
        QCOMPARE(hotkeyCheck(env).outcome, Outcome::Warn);
        QVERIFY(hotkeyCheck(env).advice.contains(QLatin1String("clipitto --toggle")));
        env.status[QStringLiteral("shortcut")] = QStringLiteral("not set: gsettings failed");
        QCOMPARE(hotkeyCheck(env).outcome, Outcome::Fail);
        env.session.display = SessionEnv::Display::Windows;
        env.status[QStringLiteral("hotkey")] = QStringLiteral("unavailable: already registered by another program");
        const Result conflict = hotkeyCheck(env);
        QCOMPARE(conflict.outcome, Outcome::Fail);
        QVERIFY(conflict.detail.contains(QLatin1String("already registered")));
        env.status[QStringLiteral("hotkey")] = QStringLiteral("Ctrl+Alt+V");
        QCOMPARE(hotkeyCheck(env).outcome, Outcome::Pass);
        env.config.panicKey = true;
        env.status[QStringLiteral("panic_hotkey")] = QStringLiteral("unavailable: in use");
        QCOMPARE(hotkeyCheck(env).outcome, Outcome::Warn);
        env.status.remove(QStringLiteral("hotkey"));
        QCOMPARE(hotkeyCheck(env).outcome, Outcome::Warn); // not registered yet
    }

    void paste()
    {
        Env env = baseEnv();
        QCOMPARE(pasteResult(PasteOutcome::Received, env).outcome, Outcome::Pass);
        QCOMPARE(pasteResult(PasteOutcome::NotReceived, env).outcome, Outcome::Fail);
        QCOMPARE(pasteResult(PasteOutcome::CannotType, env).outcome, Outcome::Warn);
        QCOMPARE(pasteResult(PasteOutcome::Disabled, env).outcome, Outcome::Skipped);
        // Without the Settings window's box (the CLI): skipped, and it says where to run it.
        const Result cli = pasteWithoutBoxCheck(env);
        QCOMPARE(cli.outcome, Outcome::Skipped);
        QVERIFY(cli.detail.contains(QLatin1String("Settings")));
        env.session.display = SessionEnv::Display::MacOS;
        env.status[QStringLiteral("accessibility")] = QStringLiteral("not granted: x");
        QCOMPARE(pasteWithoutBoxCheck(env).outcome, Outcome::Warn);
        env.status[QStringLiteral("accessibility")] = QStringLiteral("granted");
        QCOMPARE(pasteWithoutBoxCheck(env).outcome, Outcome::Pass);
        env.config.pasteOnEnter = false;
        QCOMPARE(pasteWithoutBoxCheck(env).outcome, Outcome::Skipped);
    }

    void database()
    {
        Env env = baseEnv();
        DbFacts good;
        good.opened = good.writable = true;
        good.quickCheck = QStringLiteral("ok");
        good.schema = good.expectedSchema = 11;
        good.freeBytes = 5LL << 30;
        good.fileBytes = 2 << 20;
        QCOMPARE(databaseResult(good, env).outcome, Outcome::Pass);
        QVERIFY(databaseResult(good, env).detail.contains(QLatin1String("/data/history.db")));
        DbFacts low = good;
        low.freeBytes = 50LL << 20;
        QCOMPARE(databaseResult(low, env).outcome, Outcome::Warn);
        low.freeBytes = 1 << 20;
        QCOMPARE(databaseResult(low, env).outcome, Outcome::Fail);
        DbFacts bad = good;
        bad.quickCheck = QStringLiteral("wrong # of entries in index x");
        QCOMPARE(databaseResult(bad, env).outcome, Outcome::Fail);
        DbFacts readOnly = good;
        readOnly.writable = false;
        readOnly.writeError = QStringLiteral("database is locked");
        QCOMPARE(databaseResult(readOnly, env).outcome, Outcome::Fail);
        DbFacts newer = good;
        newer.schema = 99;
        QCOMPARE(databaseResult(newer, env).outcome, Outcome::Fail);
        DbFacts closed;
        closed.error = QStringLiteral("file is not a database");
        QCOMPARE(databaseResult(closed, env).outcome, Outcome::Fail);
        env.historyConverting = true;
        QCOMPARE(databaseResult(good, env).outcome, Outcome::Skipped);
    }

    void configAndLog()
    {
        Env env = baseEnv();
        QCOMPARE(configResult({false, {}}, env).outcome, Outcome::Pass);
        QCOMPARE(configResult({true, {}}, env).outcome, Outcome::Pass);
        const Result warned = configResult({true, {QStringLiteral("unknown key 'foo'"), QStringLiteral("hotkey: expected a shortcut")}}, env);
        QCOMPARE(warned.outcome, Outcome::Warn);
        QVERIFY(warned.detail.contains(QLatin1String("unknown key 'foo'")));
        QVERIFY(warned.detail.contains(QLatin1String("2 problem")));
        QCOMPARE(logResult({true, {}}, env).outcome, Outcome::Pass);
        QVERIFY(logResult({true, {}}, env).detail.contains(QLatin1String("/data/debug.log")));
        QCOMPARE(logResult({false, QStringLiteral("denied")}, env).outcome, Outcome::Warn); // log off
        env.config.debugLog = true;
        QCOMPARE(logResult({false, QStringLiteral("denied")}, env).outcome, Outcome::Fail); // log on
    }

    void keyrings()
    {
        // Not reachable and not needed: skipped, with why. Reachable and not needed: pass without a read.
        FakeKeyStore store;
        store.isAvailable = false;
        QCOMPARE(keyringCheck(&store, false, true, QStringLiteral("k"), QStringLiteral("sync")).outcome, Outcome::Skipped);
        QCOMPARE(keyringCheck(&store, true, true, QStringLiteral("k"), QStringLiteral("sync")).outcome, Outcome::Fail);
        store.isAvailable = true;
        const Result idle = keyringCheck(&store, false, true, QStringLiteral("k"), QStringLiteral("sync"));
        QCOMPARE(idle.outcome, Outcome::Pass);
        // Needed: a real read.
        QCOMPARE(keyringCheck(&store, true, true, QStringLiteral("history key"), QStringLiteral("history encryption")).outcome,
                 Outcome::Fail); // reachable, no key stored: the history cannot be opened
        QCOMPARE(keyringCheck(&store, true, false, QStringLiteral("match key"), QStringLiteral("password copies")).outcome, Outcome::Warn);
        store.stored = QByteArray("key");
        QCOMPARE(keyringCheck(&store, true, true, QStringLiteral("history key"), QStringLiteral("history encryption")).outcome,
                 Outcome::Pass);
        // No store at all.
        QCOMPARE(keyringCheck(nullptr, false, true, QStringLiteral("k"), QStringLiteral("sync")).outcome, Outcome::Skipped);
        QCOMPARE(keyringCheck(nullptr, true, true, QStringLiteral("k"), QStringLiteral("sync")).outcome, Outcome::Fail);
    }

    void aKeyringCheckThatBlocksIsCaughtByTheRunner()
    {
        struct Stuck : FakeKeyStore {
            std::optional<QByteArray> read(QString*) override
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                return std::nullopt;
            }
        };
        auto stuck = std::make_shared<Stuck>();
        Env env = baseEnv();
        env.historyKeys = stuck;
        env.historyKeyNeeded = true;
        QList<Check> checks = buildChecks(env);
        // The real list gives a keyring 40 s; shorten it, keeping the check itself.
        QList<Check> only;
        for (Check c : checks) {
            if (c.id == QLatin1String("keyring_history")) {
                c.timeoutMs = 300;
                only.push_back(c);
            }
        }
        QCOMPARE(only.size(), 1);
        Runner runner(only, QString());
        QSignalSpy done(&runner, &Runner::finished);
        runner.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 20000);
        const Report report = qvariant_cast<Report>(done.first().at(0));
        QCOMPARE(report.results.at(0).outcome, Outcome::Fail);
        QVERIFY(report.results.at(0).detail.startsWith(QLatin1String("did not answer in ")));
        QTest::qWait(1700); // let the blocked thread end before the store goes
    }

    void passwordChain()
    {
        Env env = baseEnv();
        QVERIFY(passwordChainCheck(env).outcome == Outcome::Skipped); // no keeper
        env.passwordsAvailable = true;
        env.passwordState = QStringLiteral("none (no master password)");
        const Result none = passwordChainCheck(env);
        QCOMPARE(none.outcome, Outcome::Warn);
        QVERIFY(none.advice.contains(QLatin1String("master password")));
        env.passwordsSetUp = true;
        env.canMatch = true;
        env.passwordState = QStringLiteral("locked");
        env.lastNameBox = QStringLiteral("named at 10:00:00");
        const Result ok = passwordChainCheck(env);
        QCOMPARE(ok.outcome, Outcome::Pass);
        for (const char* part : {"master password: locked", "name box after a password copy: on", "match key: available",
                                 "detect_secrets on", "detect_passwords off", "last name box: named at 10:00:00"}) {
            QVERIFY2(ok.detail.contains(QLatin1String(part)), part);
        }
        // The reasons come from the real rule, on samples of our own.
        QVERIFY(ok.detail.contains(QLatin1String("a mixed random string: kept")));
        QVERIFY(ok.detail.contains(QLatin1String("words joined together: not kept (looks like words and numbers joined together)")));
        QVERIFY(ok.detail.contains(QLatin1String("a short string: not kept (length outside 8 to 64)")));
        env.canMatch = false;
        env.matchProblem = QStringLiteral("keyring locked");
        const Result noMatch = passwordChainCheck(env);
        QCOMPARE(noMatch.outcome, Outcome::Warn);
        QVERIFY(noMatch.detail.contains(QLatin1String("keyring locked")));
        env.canMatch = true;
        env.config.keepPasswords = false;
        QCOMPARE(passwordChainCheck(env).outcome, Outcome::Warn);
    }

    void pasteTarget()
    {
        Env env = baseEnv();
        QCOMPARE(pasteTargetCheck(env).outcome, Outcome::Skipped); // no way to tell
        env.pasteTarget = [] { return PasteTarget::unsupported(); };
        QCOMPARE(pasteTargetCheck(env).outcome, Outcome::Skipped);
        env.pasteTarget = [] { return PasteTarget::unknown(); };
        QCOMPARE(pasteTargetCheck(env).outcome, Outcome::Skipped);
        env.pasteTarget = [] { return PasteTarget::known({QStringLiteral("gnome-terminal-server"), QStringLiteral("Gnome-terminal")}); };
        const Result terminal = pasteTargetCheck(env);
        QCOMPARE(terminal.outcome, Outcome::Pass);
        QVERIFY2(terminal.detail.contains(QLatin1String("built-in rule")), qPrintable(terminal.detail));
        QVERIFY(terminal.detail.contains(QLatin1String("ctrl+shift+v")));
        env.pasteTarget = [] { return PasteTarget::known({QStringLiteral("someeditor")}); };
        QVERIFY(pasteTargetCheck(env).detail.contains(QLatin1String("no rule matched")));
        env.config.pasteKeysApps = {{QStringLiteral("someeditor"), PasteKeys::ShiftInsert}};
        QVERIFY(pasteTargetCheck(env).detail.contains(QLatin1String("your rule")));
    }

    void gnomeExtension()
    {
        QCOMPARE(gnomeExtensionResult(ExtensionAnswer::Answers).outcome, Outcome::Pass);
        QCOMPARE(gnomeExtensionResult(ExtensionAnswer::TooOld).outcome, Outcome::Warn);
        const Result off = gnomeExtensionResult(ExtensionAnswer::NotAnswering);
        QCOMPARE(off.outcome, Outcome::Fail);
        QVERIFY(off.advice.contains(QLatin1String("gnome-extensions enable")));
        // Only on GNOME with Wayland the check is live; elsewhere it is skipped.
        Env env = baseEnv();
        QCOMPARE(runAll(env).find(QStringLiteral("gnome_extension"))->outcome, Outcome::Skipped);
        env.session = {SessionEnv::Display::Wayland, true};
        env.extensionProbe = [](std::function<void(ExtensionAnswer)> done) { done(ExtensionAnswer::TooOld); };
        QCOMPARE(runAll(env).find(QStringLiteral("gnome_extension"))->outcome, Outcome::Warn);
    }

    void screenLock()
    {
        Env env = baseEnv();
        QCOMPARE(screenLockCheck(env).outcome, Outcome::Skipped);
        env.passwordsAvailable = true;
        env.config.lockOnScreenLock = false;
        QCOMPARE(screenLockCheck(env).outcome, Outcome::Skipped);
        env.config.lockOnScreenLock = true;
        QCOMPARE(screenLockCheck(env).outcome, Outcome::Warn);
        env.lockWatching = true;
        QCOMPARE(screenLockCheck(env).outcome, Outcome::Pass);
    }

    void translationAndOcr()
    {
        Env env = baseEnv();
        TranslationFacts t;
        QCOMPARE(translationResult(t).outcome, Outcome::Skipped); // no engine
        t.builtIn = true;
        QCOMPARE(translationResult(t).outcome, Outcome::Skipped); // none installed
        t.packs = {{QStringLiteral("he-en"), true}, {QStringLiteral("en-he"), true}};
        const Result ok = translationResult(t);
        QCOMPARE(ok.outcome, Outcome::Pass);
        QVERIFY(ok.detail.contains(QLatin1String("he-en, en-he")));
        t.packs.push_back({QStringLiteral("zh-en"), false});
        const Result damaged = translationResult(t);
        QCOMPARE(damaged.outcome, Outcome::Warn);
        QVERIFY(damaged.detail.contains(QLatin1String("cannot be loaded: zh-en")));

        OcrFacts o;
        QCOMPARE(ocrResult(o, env).outcome, Outcome::Skipped);
        o.builtIn = true;
        o.why = QStringLiteral("no language data");
        QCOMPARE(ocrResult(o, env).outcome, Outcome::Warn);
        o.available = true;
        o.languages = {QStringLiteral("en"), QStringLiteral("he")};
        const Result ready = ocrResult(o, env);
        QCOMPARE(ready.outcome, Outcome::Pass);
        QVERIFY(ready.detail.contains(QLatin1String("en, he")));
    }

    void sync()
    {
        Env env = baseEnv();
        QCOMPARE(syncResult({}, env).outcome, Outcome::Skipped);
        env.config.syncEnabled = true;
        env.config.syncFolder = QStringLiteral("/s");
        env.status[QStringLiteral("sync")] = QStringLiteral("on, 1 other device(s); last sync 10:00");
        QCOMPARE(syncResult({false, false, false, true}, env).outcome, Outcome::Fail);
        QCOMPARE(syncResult({true, false, false, true}, env).outcome, Outcome::Fail);
        QCOMPARE(syncResult({true, true, false, true}, env).outcome, Outcome::Fail);
        QCOMPARE(syncResult({true, true, true, false}, env).outcome, Outcome::Fail);
        QCOMPARE(syncResult({true, true, true, true}, env).outcome, Outcome::Pass);
        env.status[QStringLiteral("sync")] = QStringLiteral("on, 0 other device(s); waiting: the folder is full");
        QCOMPARE(syncResult({true, true, true, true}, env).outcome, Outcome::Warn);
    }

    void forgetPanicAndHello()
    {
        Env env = baseEnv();
        QVERIFY(forgetPanicCheck(env).detail.contains(QLatin1String("auto-forget: off; panic key: off")));
        env.config.panicKey = true;
        QVERIFY(forgetPanicCheck(env).detail.contains(QLatin1String("panic key: on")));
        env.helloName = QStringLiteral("Windows Hello");
        QCOMPARE(helloResult(true, {}, env).outcome, Outcome::Pass);
        QCOMPARE(helloResult(false, QStringLiteral("no PIN"), env).outcome, Outcome::Skipped);
        env.config.windowsHello = true;
        QCOMPARE(helloResult(false, QStringLiteral("no PIN"), env).outcome, Outcome::Warn);
        // Linux has no such feature: the check is in the list, skipped.
        QCOMPARE(runAll(baseEnv()).find(QStringLiteral("hello"))->outcome, Outcome::Skipped);
    }

    // --- the real probes --------------------------------------------------------------------------

    void databaseProbeOnARealFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("history.db"));
        {
            db::Db database(path);
            db::configureConnection(database);
            db::migrate(database);
        } // closed before the probe and before the directory goes (Windows)
        const DbFacts facts = probeDatabase(path);
        QVERIFY2(facts.opened, qPrintable(facts.error));
        QCOMPARE(facts.quickCheck, QStringLiteral("ok"));
        QVERIFY(facts.schema > 0);
        QCOMPARE(facts.schema, facts.expectedSchema);
        QVERIFY(facts.writable);
        QVERIFY(facts.fileBytes > 0);
        QVERIFY(facts.freeBytes != 0);
        Env env = baseEnv();
        env.databasePath = path;
        QCOMPARE(databaseResult(facts, env).outcome, Outcome::Pass);

        // Not a database.
        const QString junk = dir.filePath(QStringLiteral("junk.db"));
        {
            QFile file(junk);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(4096, 'x'));
        }
        const DbFacts broken = probeDatabase(junk);
        QCOMPARE(databaseResult(broken, env).outcome, Outcome::Fail);
        // Missing file.
        QVERIFY(!probeDatabase(dir.filePath(QStringLiteral("missing.db"))).opened);
    }

    void configProbeListsWarningsWithoutValues()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("config.toml"));
        QVERIFY(!probeConfig(path).exists);
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QStringLiteral("max_history = 99999999\nhotkey = \"%1\"\nnot_a_key = \"%1\"\n").arg(kSecret).toUtf8());
        }
        const ConfigFacts facts = probeConfig(path);
        QVERIFY(facts.exists);
        QVERIFY(facts.warnings.size() >= 3);
        QVERIFY(facts.warnings.join(QLatin1Char(' ')).contains(QLatin1String("not_a_key")));
        QVERIFY2(!facts.warnings.join(QLatin1Char(' ')).contains(kSecret), qPrintable(facts.warnings.join(QLatin1Char('|'))));
        Env env = baseEnv();
        env.configPath = path;
        QCOMPARE(configResult(facts, env).outcome, Outcome::Warn);
    }

    void logProbe()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("sub/debug.log"));
        QVERIFY(probeLogFile(log).writable);
        QVERIFY(!QFile::exists(log + QStringLiteral(".selftest"))); // its file is gone again (and the log itself untouched)
        QVERIFY(!QFile::exists(log));
        // A "folder" that is a file: nothing can be made under it.
        const QString blocker = dir.filePath(QStringLiteral("blocker"));
        {
            QFile file(blocker);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        const LogFacts blocked = probeLogFile(blocker + QStringLiteral("/debug.log"));
        QVERIFY(!blocked.writable);
        QVERIFY(!blocked.error.isEmpty());
    }

    void syncFolderProbe()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(!probeSyncFolder(QString(), true).folderSet);
        const SyncFacts missing = probeSyncFolder(dir.filePath(QStringLiteral("nope")), true);
        QVERIFY(missing.folderSet && !missing.folderExists);
        const SyncFacts ok = probeSyncFolder(dir.path(), false);
        QVERIFY(ok.folderExists && ok.folderWritable && !ok.passphrasePresent);
    }
};

QTEST_MAIN(TestSelfTest)
#include "tst_selftest.moc"
