#include "LocalClient.h"
#include "LocalControlChannel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>

#include <future>

using clipitto::ControlChannel;
using clipitto::localipc::LocalControlChannel;
using clipitto::localipc::Reply;
using clipitto::localipc::Request;

// The local-socket control channel (Windows' transport) and the CLI built on it.
class TestLocalIpc : public QObject {
    Q_OBJECT

private:
    // A unique name per test run, so tests never meet a real daemon.
    const QString m_name = QStringLiteral("clipitto-test-%1").arg(QCoreApplication::applicationPid());

    // The client blocks, so it runs on another thread while this one serves requests.
    Reply call(const Request& request)
    {
        auto future = std::async(std::launch::async, [&] { return clipitto::localipc::call(m_name, request, 5000); });
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return future.get();
    }

    std::pair<int, QString> runCli(const QStringList& args)
    {
        QProcess cli;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("CLIPITTO_SERVER_NAME"), m_name);
        cli.setProcessEnvironment(env);
        cli.start(QStringLiteral(CLIPITTO_CLI_PATH), args);
        QSignalSpy finished(&cli, &QProcess::finished);
        if (!finished.wait(10000)) {
            cli.kill();
            return {-1, QStringLiteral("timeout")};
        }
        return {cli.exitCode(), QString::fromUtf8(cli.readAllStandardOutput() + cli.readAllStandardError())};
    }

private slots:
    void singleInstance()
    {
        LocalControlChannel first(m_name);
        QVERIFY(first.start() == ControlChannel::StartResult::Ok);
        LocalControlChannel second(m_name);
        QString error;
        QVERIFY(second.start(&error) == ControlChannel::StartResult::AlreadyRunning);
        QVERIFY(error.contains(m_name));
    }

    void requestsBecomeSignals()
    {
        LocalControlChannel channel(m_name);
        QVERIFY(channel.start() == ControlChannel::StartResult::Ok);
        QSignalSpy toggled(&channel, &ControlChannel::toggleRequested);
        QSignalSpy ingested(&channel, &ControlChannel::ingestRequested);
        QSignalSpy hidden(&channel, &ControlChannel::hideRequested);
        QSignalSpy reloaded(&channel, &ControlChannel::reloadRequested);
        QSignalSpy quit(&channel, &ControlChannel::quitRequested);

        QVERIFY(call({QStringLiteral("Toggle"), {QStringLiteral("token")}}).ok);
        QCOMPARE(toggled.size(), 1);
        QCOMPARE(toggled.at(0).at(0).toString(), QStringLiteral("token"));

        QVERIFY(call({QStringLiteral("Ingest"), {QStringLiteral("text"), QJsonArray{QStringLiteral("text/plain")}}}).ok);
        QCOMPARE(ingested.at(0).at(0).toString(), QStringLiteral("text"));
        QCOMPARE(ingested.at(0).at(1).toStringList(), QStringList{QStringLiteral("text/plain")});

        QVERIFY(call({QStringLiteral("Hide"), {}}).ok);
        QVERIFY(call({QStringLiteral("ReloadConfig"), {}}).ok);
        QCOMPARE(hidden.size(), 1);
        QCOMPARE(reloaded.size(), 1);

        QVERIFY(call({QStringLiteral("Quit"), {}}).ok);
        QTRY_COMPARE(quit.size(), 1); // delivered after the reply
    }

    void statusReturnsTheProvidersMap()
    {
        LocalControlChannel channel(m_name);
        channel.setStatusProvider([] { return QVariantMap{{QStringLiteral("items"), 3}, {QStringLiteral("visible"), true}}; });
        QVERIFY(channel.start() == ControlChannel::StartResult::Ok);
        const Reply reply = call({QStringLiteral("Status"), {}});
        QVERIFY(reply.ok);
        QCOMPARE(reply.result.toObject().value(QStringLiteral("items")).toInt(), 3);
        QCOMPARE(reply.result.toObject().value(QStringLiteral("visible")).toBool(), true);
    }

    void badRequestsGetErrorsAndTheConnectionSurvives()
    {
        LocalControlChannel channel(m_name);
        QVERIFY(channel.start() == ControlChannel::StartResult::Ok);
        const Reply unknown = call({QStringLiteral("FormatDisk"), {}});
        QVERIFY(!unknown.ok);
        QVERIFY(unknown.error.contains(QLatin1String("unknown method")));

        // Raw socket: garbage then a valid request on the same connection.
        auto future = std::async(std::launch::async, [this] {
            QLocalSocket socket;
            socket.connectToServer(m_name);
            socket.waitForConnected(2000);
            socket.write("this is not json\n{\"method\":\"Hide\",\"args\":[]}\n");
            socket.flush();
            QByteArray data;
            while (data.count('\n') < 2 && socket.waitForReadyRead(2000)) {
                data += socket.readAll();
            }
            return data;
        });
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        const QList<QByteArray> lines = future.get().trimmed().split('\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(!clipitto::localipc::decodeReply(lines[0]).ok);
        QVERIFY(clipitto::localipc::decodeReply(lines[1]).ok);
    }

    void clientReportsAMissingDaemon()
    {
        const Reply reply = clipitto::localipc::call(m_name + QStringLiteral("-nobody"), {QStringLiteral("Status"), {}}, 1000);
        QVERIFY(!reply.ok);
        QCOMPARE(reply.error, QStringLiteral("the daemon is not running"));
    }

    void cliTalksToTheChannel()
    {
        LocalControlChannel channel(m_name);
        channel.setStatusProvider([] { return QVariantMap{{QStringLiteral("backend"), QStringLiteral("qt-clipboard")}}; });
        QVERIFY(channel.start() == ControlChannel::StartResult::Ok);
        QSignalSpy shown(&channel, &ControlChannel::showRequested);
        QSignalSpy quit(&channel, &ControlChannel::quitRequested);

        auto [code, output] = runCli({QStringLiteral("--status")});
        QVERIFY2(code == 0, qPrintable(output));
        QVERIFY(output.contains(QLatin1String("backend: qt-clipboard")));

        std::tie(code, output) = runCli({QStringLiteral("--show")});
        QVERIFY2(code == 0, qPrintable(output));
        QCOMPARE(shown.size(), 1);

        std::tie(code, output) = runCli({QStringLiteral("--quit")});
        QVERIFY2(code == 0, qPrintable(output));
        QTRY_COMPARE(quit.size(), 1);
    }

    void cliReportsAMissingDaemon()
    {
        const auto [code, output] = runCli({QStringLiteral("--toggle")});
        QCOMPARE(code, 1);
        QVERIFY(output.contains(QLatin1String("the daemon is not running")));
    }
};

QTEST_GUILESS_MAIN(TestLocalIpc)
#include "tst_localipc.moc"
