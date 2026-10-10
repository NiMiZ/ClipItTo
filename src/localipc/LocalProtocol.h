#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonValue>
#include <QString>

#include <optional>

// Request/reply protocol over QLocalSocket (a named pipe on Windows, a Unix socket elsewhere):
// one compact JSON object per line.
//   request: {"method": "Toggle", "args": ["token"]}
//   reply:   {"ok": true, "result": ...}  or  {"ok": false, "error": "message"}
namespace clipitto::localipc {

// Per-user server name. CLIPITTO_SERVER_NAME overrides it (tests, side-by-side instances).
QString defaultServerName();

struct Request {
    QString method;
    QJsonArray args;
};

struct Reply {
    bool ok = false;
    QJsonValue result;
    QString error;
};

QByteArray encodeRequest(const Request& request);
std::optional<Request> decodeRequest(const QByteArray& line, QString* error = nullptr);

QByteArray encodeReply(const Reply& reply);
Reply decodeReply(const QByteArray& line);

// Requests larger than this are rejected (a clip is capped well below it).
inline constexpr qint64 kMaxLineBytes = 64 * 1024 * 1024;

} // namespace clipitto::localipc
