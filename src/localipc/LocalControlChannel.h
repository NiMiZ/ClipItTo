#pragma once

#include "ControlChannel.h"
#include "LocalProtocol.h"

class QLocalServer;
class QLocalSocket;

namespace clipitto::localipc {

// ControlChannel over QLocalServer: the daemon's endpoint on Windows (named pipe), and in
// builds without D-Bus elsewhere (Unix socket). Also enforces a single daemon per user.
class LocalControlChannel : public ControlChannel {
    Q_OBJECT

public:
    explicit LocalControlChannel(QString serverName = defaultServerName(), QObject* parent = nullptr);
    ~LocalControlChannel() override;

    StartResult start(QString* error = nullptr) override;
    QString serverName() const { return m_serverName; }

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);
    Reply dispatch(const Request& request);

    QString m_serverName;
    QLocalServer* m_server;
};

} // namespace clipitto::localipc
