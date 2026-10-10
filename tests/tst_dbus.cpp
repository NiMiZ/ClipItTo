#include "Daemon.h"
#include "DbusControlChannel.h"
#include "DbusNames.h"
#include "db/Repository.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using clipitto::Daemon;
using clipitto::DaemonOptions;
using clipitto::ipc::DbusControlChannel;

// Integration tests against a real session bus. ctest runs this under dbus-run-session,
// so it never touches the developer's own bus or a running clipitto-daemon.
class TestDbus : public QObject {
    Q_OBJECT

private:
    static QDBusMessage call(const QString& method, const QVariantList& args = {})
    {
        auto msg = QDBusMessage::createMethodCall(QString::fromLatin1(clipitto::ipc::kServiceName),
                                                  QString::fromLatin1(clipitto::ipc::kObjectPath),
                                                  QString::fromLatin1(clipitto::ipc::kInterface), method);
        msg.setArguments(args);
        // Asynchronous call + wait keeps this thread's event loop serving the daemon side.
        QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(msg, 5000);
        QDBusPendingCallWatcher watcher(pending);
        QSignalSpy finished(&watcher, &QDBusPendingCallWatcher::finished);
        if (!watcher.isFinished()) {
            finished.wait(5000);
        }
        return pending.reply();
    }

    // Runs the real CLI binary while our event loop keeps answering its D-Bus calls.
    static std::pair<int, QString> runCli(const QStringList& args)
    {
        QProcess cli;
        cli.start(QStringLiteral(CLIPITTO_CLI_PATH), args);
        QSignalSpy finished(&cli, &QProcess::finished);
        if (!finished.wait(10000)) {
            cli.kill();
            return {-1, QStringLiteral("timeout")};
        }
        return {cli.exitCode(), QString::fromUtf8(cli.readAllStandardOutput() + cli.readAllStandardError())};
    }

    QTemporaryDir m_dir;
    std::unique_ptr<Daemon> m_daemon;
    int m_lastVisibility = -1;

public slots:
    void onVisibilityChanged(bool visible) { m_lastVisibility = visible ? 1 : 0; }

private slots:
    void initTestCase()
    {
        if (!QDBusConnection::sessionBus().isConnected()) {
            QSKIP("no D-Bus session bus (run via ctest, which wraps this in dbus-run-session)");
        }
        QVERIFY(m_dir.isValid());
        DaemonOptions options;
        options.configPath = m_dir.filePath(QStringLiteral("config.toml"));
        options.databasePath = m_dir.filePath(QStringLiteral("data/history.db"));
        m_daemon = std::make_unique<Daemon>(options, std::make_unique<DbusControlChannel>(QDBusConnection::sessionBus()));
        QString error;
        QVERIFY2(m_daemon->start(&error) == Daemon::StartResult::Ok, qPrintable(error));
    }

    void secondInstanceIsRejected()
    {
        DaemonOptions options;
        options.configPath = m_dir.filePath(QStringLiteral("config.toml"));
        options.databasePath = m_dir.filePath(QStringLiteral("other.db"));
        // A separate connection, as a second process would have.
        Daemon second(options, std::make_unique<DbusControlChannel>(
                                   QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("second"))));
        QVERIFY(second.start() == Daemon::StartResult::AlreadyRunning);
        QVERIFY(!QFile::exists(options.databasePath)); // bailed out before opening a database
        QDBusConnection::disconnectFromBus(QStringLiteral("second"));
    }

    void ingestOverDbusStoresText()
    {
        const auto reply = call(QStringLiteral("Ingest"),
                                {QStringLiteral("from dbus"), QStringList{QStringLiteral("text/plain")}});
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(m_daemon->repository()->recent(1, false).front().content, QStringLiteral("from dbus"));
    }

    void ingestRespectsSensitiveHint()
    {
        const qint64 before = m_daemon->repository()->count();
        call(QStringLiteral("Ingest"), {QStringLiteral("hunter2"), QStringList{QStringLiteral("x-kde-passwordManagerHint")}});
        QCOMPARE(m_daemon->repository()->count(), before);
    }

    void statusReportsState()
    {
        const auto reply = call(QStringLiteral("Status"));
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        const QVariantMap status = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
        QCOMPARE(status.value(QStringLiteral("items")).toLongLong(), m_daemon->repository()->count());
        QCOMPARE(status.value(QStringLiteral("backend")).toString(), QStringLiteral("qt-clipboard"));
        QCOMPARE(status.value(QStringLiteral("max_history")).toInt(), 1000);
    }

    void reloadConfigMethodTriggersReload()
    {
        QFile file(m_dir.filePath(QStringLiteral("config.toml")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("max_history = 10\n");
        file.close();

        QCOMPARE(call(QStringLiteral("ReloadConfig")).type(), QDBusMessage::ReplyMessage);
        QCOMPARE(m_daemon->config().maxHistory, 10);
        QFile::remove(file.fileName());
        call(QStringLiteral("ReloadConfig"));
    }

    void visibilityIsBroadcast()
    {
        QVERIFY(QDBusConnection::sessionBus().connect(
            QString(), QString::fromLatin1(clipitto::ipc::kObjectPath), QString::fromLatin1(clipitto::ipc::kInterface),
            QStringLiteral("VisibilityChanged"), this, SLOT(onVisibilityChanged(bool))));
        emit m_daemon->control()->visibilityChanged(true);
        QTRY_COMPARE(m_lastVisibility, 1);
    }

    void cliToggleReachesDaemon()
    {
        const auto [code, output] = runCli({QStringLiteral("--toggle")});
        QVERIFY2(code == 0, qPrintable(output));
    }

    void cliStatusPrintsKeys()
    {
        const auto [code, output] = runCli({QStringLiteral("--status")});
        QVERIFY2(code == 0, qPrintable(output));
        QVERIFY(output.contains(QLatin1String("backend: qt-clipboard")));
        QVERIFY(output.contains(QLatin1String("items: ")));
    }

    void cliIngestStoresText()
    {
        const auto [code, output] = runCli({QStringLiteral("--ingest"), QStringLiteral("via cli")});
        QVERIFY2(code == 0, qPrintable(output));
        QCOMPARE(m_daemon->repository()->recent(1, false).front().content, QStringLiteral("via cli"));
    }

    void cliRejectsZeroOrMultipleActions()
    {
        QCOMPARE(runCli({}).first, 2);
        QCOMPARE(runCli({QStringLiteral("--show"), QStringLiteral("--hide")}).first, 2);
    }

    void cliReportsUnreachableDaemon()
    {
        m_daemon.reset(); // releases the bus name
        const auto [code, output] = runCli({QStringLiteral("--status")});
        QCOMPARE(code, 1);
        QVERIFY(output.contains(QLatin1String("cannot reach the daemon")));
    }
};

QTEST_MAIN(TestDbus)
#include "tst_dbus.moc"
