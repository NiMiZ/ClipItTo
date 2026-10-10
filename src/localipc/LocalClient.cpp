#include "LocalClient.h"

#include <QElapsedTimer>
#include <QLocalSocket>

namespace clipitto::localipc {

Reply call(const QString& serverName, const Request& request, int timeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(serverName);
    if (!socket.waitForConnected(timeoutMs)) {
        const auto e = socket.error();
        if (e == QLocalSocket::ServerNotFoundError || e == QLocalSocket::ConnectionRefusedError) {
            return {false, {}, QStringLiteral("the daemon is not running")};
        }
        return {false, {}, socket.errorString()};
    }
    socket.write(encodeRequest(request));
    socket.flush();

    QElapsedTimer timer;
    timer.start();
    QByteArray buffer;
    while (!buffer.contains('\n')) {
        const qint64 remaining = timeoutMs - timer.elapsed();
        if (remaining <= 0 || !socket.waitForReadyRead(int(remaining))) {
            return {false, {}, QStringLiteral("no reply from the daemon (%1)").arg(socket.errorString())};
        }
        buffer += socket.readAll();
    }
    return decodeReply(buffer.left(buffer.indexOf('\n')));
}

} // namespace clipitto::localipc
