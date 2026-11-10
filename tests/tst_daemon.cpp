#include "ClipFilter.h"
#include "ControlChannel.h"
#include "Daemon.h"
#include "DesktopIntegration.h"
#include "FakeKeyStore.h"
#include "db/Sqlite.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "SearchBar.h"
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

using clipitto::Config;
using clipitto::ControlChannel;
using clipitto::DesktopIntegration;
using clipitto::PasteKeys;
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

    void setAvailable(bool available)
    {
        m_available = available;
        emit availabilityChanged(available);
    }

    int activations = 0;
    QStringList clipboardTexts;
    QList<QByteArray> clipboardImages;
    QList<PasteKeys> pastes;

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
};

QTEST_MAIN(TestDaemon)
#include "tst_daemon.moc"
