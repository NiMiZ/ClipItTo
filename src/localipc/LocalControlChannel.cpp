#include "LocalControlChannel.h"

#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

namespace clipitto::localipc {

LocalControlChannel::LocalControlChannel(QString serverName, QObject* parent)
    : ControlChannel(parent)
    , m_serverName(std::move(serverName))
    , m_server(new QLocalServer(this))
{
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &LocalControlChannel::onNewConnection);
}

LocalControlChannel::~LocalControlChannel() = default;

ControlChannel::StartResult LocalControlChannel::start(QString* error)
{
    // Ask first: on Windows two servers may listen on one pipe name, so listen() alone
    // cannot tell us that another instance is running.
    {
        QLocalSocket probe;
        probe.connectToServer(m_serverName);
        if (probe.waitForConnected(500)) {
            if (error) {
                *error = QStringLiteral("another ClipItTo daemon is listening on %1").arg(m_serverName);
            }
            return StartResult::AlreadyRunning;
        }
    }
    if (m_server->listen(m_serverName)) {
        return StartResult::Ok;
    }
    // Nobody answered, so a leftover Unix socket file from a crashed daemon is in the way.
    if (m_server->serverError() == QAbstractSocket::AddressInUseError) {
        QLocalServer::removeServer(m_serverName);
        if (m_server->listen(m_serverName)) {
            return StartResult::Ok;
        }
    }
    if (error) {
        *error = QStringLiteral("cannot listen on %1: %2").arg(m_serverName, m_server->errorString());
    }
    return StartResult::Failed;
}

void LocalControlChannel::onNewConnection()
{
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void LocalControlChannel::onReadyRead(QLocalSocket* socket)
{
    while (socket->canReadLine()) {
        const QByteArray line = socket->readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QString error;
        const auto request = decodeRequest(line, &error);
        socket->write(encodeReply(request ? dispatch(*request) : Reply{false, {}, error}));
    }
    if (socket->bytesAvailable() > kMaxLineBytes) {
        socket->write(encodeReply({false, {}, QStringLiteral("request too large")}));
        socket->disconnectFromServer();
    }
}

Reply LocalControlChannel::dispatch(const Request& request)
{
    const QString& m = request.method;
    const QString firstArg = request.args.at(0).toString();
    if (m == QLatin1String("Toggle")) {
        emit toggleRequested(firstArg);
    } else if (m == QLatin1String("Show")) {
        emit showRequested(firstArg);
    } else if (m == QLatin1String("Hide")) {
        emit hideRequested();
    } else if (m == QLatin1String("Ingest")) {
        QStringList mimeTypes;
        for (const QJsonValue& v : request.args.at(1).toArray()) {
            mimeTypes << v.toString();
        }
        emit ingestRequested(firstArg, mimeTypes);
    } else if (m == QLatin1String("ReloadConfig")) {
        emit reloadRequested();
    } else if (m == QLatin1String("Quit")) {
        // Reply first; quitting tears down this socket.
        QTimer::singleShot(0, this, &ControlChannel::quitRequested);
    } else if (m == QLatin1String("Status")) {
        return {true, QJsonObject::fromVariantMap(status()), {}};
    } else {
        return {false, {}, QStringLiteral("unknown method %1").arg(m)};
    }
    return {true, {}, {}};
}

} // namespace clipitto::localipc
