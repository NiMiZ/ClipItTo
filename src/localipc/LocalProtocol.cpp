#include "LocalProtocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace clipitto::localipc {

QString defaultServerName()
{
    const QString overridden = qEnvironmentVariable("CLIPITTO_SERVER_NAME");
    if (!overridden.isEmpty()) {
        return overridden;
    }
#ifdef Q_OS_WIN
    QString user = qEnvironmentVariable("USERNAME");
#else
    QString user = qEnvironmentVariable("USER");
#endif
    // Pipe names are machine-wide on Windows: the user name keeps sessions apart.
    user.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    return QStringLiteral("clipitto-") + (user.isEmpty() ? QStringLiteral("default") : user);
}

QByteArray encodeRequest(const Request& request)
{
    const QJsonObject obj{{QStringLiteral("method"), request.method}, {QStringLiteral("args"), request.args}};
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<Request> decodeRequest(const QByteArray& line, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    const QJsonObject obj = doc.object();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()
        || !obj.value(QStringLiteral("method")).isString()) {
        if (error) {
            *error = QStringLiteral("malformed request");
        }
        return std::nullopt;
    }
    return Request{obj.value(QStringLiteral("method")).toString(), obj.value(QStringLiteral("args")).toArray()};
}

QByteArray encodeReply(const Reply& reply)
{
    QJsonObject obj{{QStringLiteral("ok"), reply.ok}};
    if (reply.ok) {
        obj.insert(QStringLiteral("result"), reply.result);
    } else {
        obj.insert(QStringLiteral("error"), reply.error);
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

Reply decodeReply(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return {false, {}, QStringLiteral("malformed reply from the daemon")};
    }
    const QJsonObject obj = doc.object();
    return {obj.value(QStringLiteral("ok")).toBool(), obj.value(QStringLiteral("result")),
            obj.value(QStringLiteral("error")).toString()};
}

} // namespace clipitto::localipc
