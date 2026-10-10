#include "ControlChannel.h"
#include "Daemon.h"
#include "HistoryModel.h"
#include "HistoryView.h"
#include "PopupWindow.h"
#include "db/Repository.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::ControlChannel;
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
