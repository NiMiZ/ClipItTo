#include "ControlChannel.h"
#include "Daemon.h"
#include "DesktopIntegration.h"
#include "FakeKeyStore.h"
#include "db/Sqlite.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "SyntheticPaste.h"
#include "db/Repository.h"

#include <QClipboard>
#include <QFile>
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
using clipitto::Daemon;
using clipitto::DaemonOptions;

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
    void paste(PasteKeys keys) override { pastes << keys; }

    void setAvailable(bool available)
    {
        m_available = available;
        emit availabilityChanged(available);
    }

    int activations = 0;
    QStringList clipboardTexts;
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
#ifdef Q_OS_WIN
        QSKIP("SendInput needs no display connection");
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
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);

        const auto stored = [&](const QString& key) {
            QFile f(store.filePath(QString(key).replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char(':'), QLatin1Char('_'))));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
        };
        const QString entry = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:"
                                             "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipitto/");

        Config c = d.config();
        c.hotkey = QStringLiteral("Ctrl+`");
        QVERIFY(d.applySettings(c));
        QCOMPARE(stored(entry + QStringLiteral(".binding")), QStringLiteral("<Control>grave"));
        QVERIFY(stored(entry + QStringLiteral(".command")).endsWith(QLatin1String("--toggle")));
        QVERIFY(stored(QStringLiteral("org.gnome.settings-daemon.plugins.media-keys.custom-keybindings"))
                    .contains(QLatin1String("/custom-keybindings/clipitto/")));
        QCOMPARE(d.status().value(QStringLiteral("shortcut")).toString(), QStringLiteral("Ctrl+` (GNOME)"));

        // Editing the file and reloading applies a changed shortcut too.
        writeConfig("hotkey = \"Ctrl+Alt+V\"\n");
        emit d.control()->reloadRequested();
        QCOMPARE(stored(entry + QStringLiteral(".binding")), QStringLiteral("<Control><Alt>v"));
#endif
    }

    void nonGnomeSessionsLeaveTheShortcutAlone()
    {
        DaemonOptions o = options();
        o.session.isGnome = false;
        o.gsettingsProgram = QStringLiteral("/nonexistent/gsettings");
        Daemon d(o, std::make_unique<FakeControl>());
        QVERIFY(d.start() == Daemon::StartResult::Ok);
        Config c = d.config();
        c.hotkey = QStringLiteral("Ctrl+`");
        QVERIFY(d.applySettings(c));
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
