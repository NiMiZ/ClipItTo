#include "PhoneServer.h"

#include "PhoneNet.h"
#include "PhonePage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStringDecoder>
#include <QTcpServer>
#include <QTcpSocket>

#include <algorithm>

// Only counts and sizes are logged here: never the token, an address, the text or a picture.
Q_LOGGING_CATEGORY(lcPhone, "clipitto.phone")

namespace clipitto::phone {

namespace {

constexpr qint64 kMaxDiscard = 1024 * 1024; // what is read and thrown away after an early answer
constexpr int kLingerMs = 2000;


const char* reasonPhrase(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

} // namespace

// One connection: reads one request, answers it, closes.
class PhoneConnection : public QObject {
public:
    PhoneConnection(PhoneServer* server, QTcpSocket* socket) : QObject(server), m_server(server), m_socket(socket)
    {
        m_socket->setParent(this);
        m_idle.setSingleShot(true);
        m_idle.setInterval(server->m_config.idleMs);
        m_life.setSingleShot(true);
        m_life.setInterval(server->m_config.maxConnectionMs);
        connect(&m_idle, &QTimer::timeout, this, [this] { close(); });
        connect(&m_life, &QTimer::timeout, this, [this] { close(); });
        connect(m_socket, &QTcpSocket::readyRead, this, [this] { onReadyRead(); });
        connect(m_socket, &QTcpSocket::bytesWritten, this, [this] { m_idle.start(); });
        connect(m_socket, &QTcpSocket::disconnected, this, [this] { finish(); });
        m_idle.start();
        m_life.start();
        if (m_socket->bytesAvailable() > 0) {
            QMetaObject::invokeMethod(this, [this] { onReadyRead(); }, Qt::QueuedConnection);
        }
    }

    ~PhoneConnection() override
    {
        if (m_reserved) {
            --m_server->m_inflight;
        }
    }

    void close()
    {
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->abort();
        }
        finish();
    }

private:
    void finish()
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_idle.stop();
        m_life.stop();
        m_server->connectionDone(this);
        deleteLater();
    }

    void onReadyRead()
    {
        if (m_finished) {
            return;
        }
        m_idle.start();
        if (m_responded) { // an early answer was given: whatever the phone still sends is thrown away
            m_discarded += m_socket->readAll().size();
            if (m_discarded > kMaxDiscard) {
                close();
            }
            return;
        }
        if (!m_headersDone) {
            m_buf.append(m_socket->readAll());
            const qsizetype end = m_buf.indexOf("\r\n\r\n");
            const qsizetype limit = m_server->m_config.maxHeaderBytes;
            if (end < 0 ? m_buf.size() > limit : end + 4 > limit) {
                respond(431);
                return;
            }
            if (end < 0) {
                return;
            }
            const QByteArray head = m_buf.left(end);
            m_body = m_buf.mid(end + 4);
            m_buf.clear();
            if (!handleHead(head)) {
                return;
            }
            m_headersDone = true;
        } else {
            m_body.append(m_socket->readAll());
        }
        if (m_body.size() >= m_length) {
            m_body.truncate(m_length);
            handleBody();
        }
    }

    // Parses the request line and headers, decides, and returns true when the body (maybe empty) is to be read.
    bool handleHead(const QByteArray& head)
    {
        QList<QByteArray> lines;
        for (qsizetype from = 0;;) {
            const qsizetype nl = head.indexOf("\r\n", from);
            if (nl < 0) {
                lines.append(head.mid(from));
                break;
            }
            lines.append(head.mid(from, nl - from));
            from = nl + 2;
        }
        const QList<QByteArray> parts = lines.first().split(' ');
        if (parts.size() != 3 || parts[0].isEmpty() || !parts[1].startsWith('/')
            || (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0")) {
            respond(400);
            return false;
        }
        for (const char c : parts[1]) {
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
                respond(400);
                return false;
            }
        }
        bool haveLength = false;
        bool expectContinue = false;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray& line = lines[i];
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0 || line.startsWith(' ') || line.startsWith('\t')) {
                respond(400);
                return false;
            }
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "transfer-encoding") {
                respond(400); // chunked bodies are not accepted (and no request smuggling)
                return false;
            }
            if (name == "content-length") {
                bool ok = false;
                const qint64 n = value.size() <= 18 ? value.toLongLong(&ok) : 0;
                if (haveLength || !ok || n < 0 || value.startsWith('+') || value.startsWith('-')) {
                    respond(400);
                    return false;
                }
                haveLength = true;
                m_length = n;
            }
            if (name == "expect" && value.toLower() == "100-continue") {
                expectContinue = true;
            }
        }

        QByteArray query;
        m_route = m_server->route(parts[0], parts[1], &query);
        m_query = query;
        switch (m_route) {
        case PhoneServer::Route::None:
            respond(404);
            return false;
        case PhoneServer::Route::Page:
        case PhoneServer::Route::Status:
            if (m_length != 0) {
                respond(400);
                return false;
            }
            return true;
        case PhoneServer::Route::Upload:
        case PhoneServer::Route::Text:
            break;
        }
        const PhoneServer::Config& cfg = m_server->m_config;
        const qint64 limit = m_route == PhoneServer::Route::Upload ? cfg.maxBodyBytes : cfg.maxTextBytes;
        if (!haveLength) {
            respond(411);
            return false;
        }
        if (m_length > limit) {
            respond(413);
            return false;
        }
        if (m_length == 0) {
            respond(400);
            return false;
        }
        if (m_server->m_uploads + m_server->m_inflight >= cfg.maxUploads) {
            respond(429);
            return false;
        }
        if (m_server->pendingItems() + m_server->m_inflight >= cfg.maxPending) {
            respond(503);
            return false;
        }
        m_reserved = true;
        ++m_server->m_inflight;
        if (expectContinue) {
            m_socket->write("HTTP/1.1 100 Continue\r\n\r\n");
        }
        return true;
    }

    void handleBody()
    {
        m_responded = true; // nothing more is read as a request
        switch (m_route) {
        case PhoneServer::Route::Page:
            send(200, "text/html; charset=utf-8", pageHtml(), /*page=*/true);
            return;
        case PhoneServer::Route::Status:
            send(200, "application/json", m_server->statusJson(m_query));
            return;
        case PhoneServer::Route::Upload:
        case PhoneServer::Route::Text:
            break;
        case PhoneServer::Route::None:
            return;
        }
        const bool text = m_route == PhoneServer::Route::Text;
        if (text) {
            QStringDecoder decoder(QStringDecoder::Utf8);
            const QString decoded = decoder(m_body);
            if (decoder.hasError() || decoded.trimmed().isEmpty()) {
                m_responded = false;
                respond(400);
                return;
            }
        }
        // Counted now: a number of uploads in one session is a hard limit.
        if (m_reserved) {
            m_reserved = false;
            --m_server->m_inflight;
        }
        const int id = m_server->m_nextId++;
        ++m_server->m_uploads;
        m_server->m_items.insert(id, {});
        send(200, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("id"), id}}).toJson(QJsonDocument::Compact));
        qCDebug(lcPhone) << "received" << (text ? "text" : "picture") << m_body.size() << "bytes, item" << id;
        QByteArray body;
        body.swap(m_body);
        QPointer<PhoneServer> server = m_server;
        emit server->received(id, text ? PhoneServer::Kind::Text : PhoneServer::Kind::Picture, body);
        body.fill('\0');
    }

    // An answer without a body of its own (errors): a bare status line, nothing about why.
    void respond(int status)
    {
        m_responded = true;
        send(status, nullptr, QByteArray());
    }

    void send(int status, const char* type, const QByteArray& body, bool page = false)
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasonPhrase(status) + "\r\n";
        if (type) {
            out += QByteArray("Content-Type: ") + type + "\r\n";
        }
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
               "Connection: close\r\n"
               "Cache-Control: no-store\r\n"
               "X-Content-Type-Options: nosniff\r\n"
               "Referrer-Policy: no-referrer\r\n"
               "Cross-Origin-Resource-Policy: same-origin\r\n";
        if (page) {
            out += "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                   "img-src blob: data:; connect-src 'self'; form-action 'none'; base-uri 'none'; frame-ancestors 'none'\r\n";
        }
        out += "\r\n";
        m_socket->write(out);
        m_socket->write(body);
        // The phone may still be sending a body we refused: it gets a moment to read the answer before the line closes
        // (closing at once, with unread data, can make the system reset the connection and drop our answer).
        const bool unread = m_length > qint64(m_body.size()) || (!m_headersDone && status == 431);
        if (unread) {
            m_responded = true;
            m_idle.setInterval(kLingerMs);
            m_idle.start();
        } else {
            m_socket->disconnectFromHost();
        }
    }

    PhoneServer* m_server;
    QTcpSocket* m_socket;
    QTimer m_idle;
    QTimer m_life;
    QByteArray m_buf;
    QByteArray m_body;
    QByteArray m_query;
    PhoneServer::Route m_route = PhoneServer::Route::None;
    qint64 m_length = 0;
    qint64 m_discarded = 0;
    bool m_headersDone = false;
    bool m_responded = false;
    bool m_reserved = false;
        bool m_finished = false;
};

PhoneServer::PhoneServer(Config config, QObject* parent)
    : QObject(parent), m_config(std::move(config)), m_server(new QTcpServer(this)), m_peerFilter(&isLocalAddress)
{
    m_server->setMaxPendingConnections(m_config.maxConnections * 2);
    m_expiry.setSingleShot(true);
    connect(&m_expiry, &QTimer::timeout, this, [this] { stop(StopReason::Expired); });
    connect(m_server, &QTcpServer::newConnection, this, &PhoneServer::onNewConnection);
}

PhoneServer::~PhoneServer()
{
    stop(StopReason::Quit);
    // Connections that are closed but not deleted yet use this object when they go.
    const QObjectList kids = children();
    for (QObject* kid : kids) {
        if (auto* connection = dynamic_cast<PhoneConnection*>(kid)) {
            delete connection;
        }
    }
}

bool PhoneServer::start(QString* error)
{
    if (m_running) {
        return true;
    }
    if (!m_server->listen(m_config.listenOn, m_config.port)) {
        if (error) {
            *error = m_server->errorString();
        }
        return false;
    }
    m_token = newToken();
    m_tokenBytes = m_token.toLatin1();
    m_uploads = 0;
    m_inflight = 0;
    m_nextId = 1;
    m_items.clear();
    m_refused = 0;
    m_running = true;
    m_deadline.setRemainingTime(m_config.sessionMs);
    m_expiry.start(m_config.sessionMs);
    qCDebug(lcPhone) << "server started";
    return true;
}

void PhoneServer::stop(StopReason reason)
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_expiry.stop();
    m_server->close();
    const QList<PhoneConnection*> open = m_connections;
    for (PhoneConnection* c : open) {
        c->close();
    }
    m_connections.clear();
    m_token.fill(QLatin1Char('0')); // not left around in memory
    m_token.clear();
    m_tokenBytes.fill('\0');
    m_tokenBytes.clear();
    qCDebug(lcPhone) << "server stopped, reason" << int(reason) << "uploads" << m_uploads << "refused connections" << m_refused;
    emit stopped(reason);
}

bool PhoneServer::extend()
{
    if (!m_running) {
        return false;
    }
    const qint64 left = std::min<qint64>(remainingMs() + m_config.extendMs, m_config.maxSessionMs);
    m_deadline.setRemainingTime(left);
    m_expiry.start(int(left));
    return true;
}

bool PhoneServer::isListening() const
{
    return m_running && m_server->isListening();
}

quint16 PhoneServer::port() const
{
    return m_server->serverPort();
}

qint64 PhoneServer::remainingMs() const
{
    return m_running ? std::max<qint64>(0, m_deadline.remainingTime()) : 0;
}

void PhoneServer::onNewConnection()
{
    while (QTcpSocket* socket = m_server->nextPendingConnection()) {
        if (!m_running || !m_peerFilter(socket->peerAddress()) || m_connections.size() >= m_config.maxConnections) {
            ++m_refused;
            socket->abort(); // no answer at all: nothing is told to a stranger
            socket->deleteLater();
            continue;
        }
        m_connections.append(new PhoneConnection(this, socket));
    }
}

void PhoneServer::connectionDone(PhoneConnection* connection)
{
    m_connections.removeAll(connection);
}

bool PhoneServer::tokenMatches(const QByteArray& segment) const
{
    return constantTimeEqual(segment, m_tokenBytes) && !m_tokenBytes.isEmpty();
}

PhoneServer::Route PhoneServer::route(const QByteArray& method, const QByteArray& target, QByteArray* query) const
{
    if (!m_running) {
        return Route::None;
    }
    const qsizetype q = target.indexOf('?');
    const QByteArray path = q < 0 ? target : target.left(q);
    *query = q < 0 ? QByteArray() : target.mid(q + 1);
    const qsizetype slash = path.indexOf('/', 1);
    const QByteArray segment = slash < 0 ? path.mid(1) : path.mid(1, slash - 1);
    const bool ok = tokenMatches(segment); // always compared, whatever the rest is
    const QByteArray rest = slash < 0 ? QByteArray() : path.mid(slash);
    if (!ok) {
        return Route::None;
    }
    if (method == "GET") {
        if (rest == "/") {
            return Route::Page;
        }
        if (rest == "/status") {
            return Route::Status;
        }
    } else if (method == "POST") {
        if (rest == "/upload") {
            return Route::Upload;
        }
        if (rest == "/text") {
            return Route::Text;
        }
    }
    return Route::None;
}

int PhoneServer::pendingItems() const
{
    int n = 0;
    for (const Item& item : m_items) {
        n += item.state == State::Sent || item.state == State::Reading;
    }
    return n;
}

void PhoneServer::setItemState(int id, State state, int chars, const QString& message)
{
    auto it = m_items.find(id);
    if (it == m_items.end()) {
        return;
    }
    it->state = state;
    it->chars = chars;
    it->message = message;
}

PhoneServer::State PhoneServer::itemState(int id, bool* known) const
{
    const auto it = m_items.constFind(id);
    if (known) {
        *known = it != m_items.constEnd();
    }
    return it == m_items.constEnd() ? State::Sent : it->state;
}

QByteArray PhoneServer::statusJson(const QByteArray& query) const
{
    int id = 0;
    for (const QByteArray& pair : query.split('&')) {
        if (pair.startsWith("id=")) {
            id = pair.mid(3).toInt();
        }
    }
    QJsonObject obj;
    if (id > 0) {
        const auto it = m_items.constFind(id);
        if (it == m_items.constEnd()) {
            obj.insert(QStringLiteral("state"), QStringLiteral("unknown"));
        } else {
            switch (it->state) {
            case State::Sent: obj.insert(QStringLiteral("state"), QStringLiteral("sent")); break;
            case State::Reading: obj.insert(QStringLiteral("state"), QStringLiteral("reading")); break;
            case State::Done:
                obj.insert(QStringLiteral("state"), QStringLiteral("done"));
                obj.insert(QStringLiteral("chars"), it->chars);
                if (!it->message.isEmpty()) {
                    obj.insert(QStringLiteral("note"), it->message);
                }
                break;
            case State::Failed:
                obj.insert(QStringLiteral("state"), QStringLiteral("failed"));
                obj.insert(QStringLiteral("error"), it->message);
                break;
            }
        }
    } else {
        obj.insert(QStringLiteral("state"), QStringLiteral("ready"));
        obj.insert(QStringLiteral("seconds"), int(remainingMs() / 1000));
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

} // namespace clipitto::phone
