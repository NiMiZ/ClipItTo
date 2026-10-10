#include "ControlChannel.h"
#include "Daemon.h"
#include "DesktopIntegration.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "X11Paster.h"
#include "db/Repository.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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
        const int olderRow = model->rowForId(d.repository()->search(QStringLiteral("older")).front().id);
        emit d.popup()->view()->activateRequested(model->index(olderRow));

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
        emit d.popup()->view()->activateRequested(d.popup()->model()->index(d.popup()->model()->rowForId(
            d.repository()->search(QStringLiteral("from history")).front().id)));
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
        emit d.popup()->view()->activateRequested(d.popup()->model()->index(d.popup()->model()->firstEntryRow()));
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
        emit d.popup()->view()->activateRequested(d.popup()->model()->index(d.popup()->model()->firstEntryRow()));
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

    void xtestPasteNeedsX11()
    {
        QVERIFY(!clipitto::pasteViaXTest(PasteKeys::CtrlV)); // offscreen platform: no X display
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
