#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>

class QTcpServer;
class QTcpSocket;

namespace clipitto::phone {

class PhoneConnection;

// The small web server behind "Receive from phone" (roadmap 28): hand-written HTTP/1.1 on QTcpServer, for the local network
// only, alive only between start() and stop(). It knows nothing about pictures: it hands the bytes it received to whoever
// listens to received() and tells the phone what that listener reports through setItemState().
//
//   GET  /<token>/                the page (PhonePage.h)
//   POST /<token>/upload          body = the picture's bytes            -> {"id":N}
//   POST /<token>/text            body = UTF-8 text                     -> {"id":N}
//   GET  /<token>/status[?id=N]   {"state":"ready",...} or the item's state
// Anything else, and any other token: a bare 404. One request per connection ("Connection: close"). No cookies, no CORS
// headers, no listing. Connections from non-local addresses are closed at once, unanswered.
class PhoneServer : public QObject {
    Q_OBJECT

public:
    struct Config {
        int maxConnections = 4;
        int idleMs = 10000; // a connection that sends or receives nothing for this long is dropped
        int maxConnectionMs = 120000; // and none lives longer than this, however steadily it trickles
        int maxHeaderBytes = 8192;
        qint64 maxBodyBytes = 20LL * 1024 * 1024; // a picture
        qint64 maxTextBytes = 100 * 1024; // typed text
        int maxUploads = 30; // pictures and texts accepted in one session
        int maxPending = 6; // received but not answered yet (the computer is still reading them): more get 503
        int sessionMs = 5 * 60 * 1000; // how long the address works
        int extendMs = 5 * 60 * 1000; // what Extend adds
        int maxSessionMs = 30 * 60 * 1000; // the most that is left after Extend
        QHostAddress listenOn = QHostAddress::AnyIPv4;
        quint16 port = 0; // 0: a random free port
    };

    enum class Kind { Picture, Text };
    enum class State { Sent, Reading, Done, Failed };
    enum class StopReason { User, Expired, Quit, Panic };

    explicit PhoneServer(Config config, QObject* parent = nullptr);
    explicit PhoneServer(QObject* parent = nullptr) : PhoneServer(Config{}, parent) {}
    ~PhoneServer() override;

    bool start(QString* error = nullptr);
    void stop(StopReason reason = StopReason::User);
    // Adds Config::extendMs to what is left (at most maxSessionMs). False when the server is not running.
    bool extend();

    bool isListening() const;
    quint16 port() const;
    // The secret part of the address. Empty when stopped. Shown to the user in the window only; never logged.
    QString token() const { return m_token; }
    qint64 remainingMs() const;
    int uploadsAccepted() const { return m_uploads; }
    int activeConnections() const { return int(m_connections.size()); }
    const Config& config() const { return m_config; }

    // Which peers are served; the default is phone::isLocalAddress. Tests replace it.
    void setPeerFilter(std::function<bool(const QHostAddress&)> filter) { m_peerFilter = std::move(filter); }

    // What the listener found out about item `id`. `chars` is the length of the text for Done; `note` or `error` a short
    // sentence for the phone ("the picture was kept, but no text recognition is available: ...").
    void setItemState(int id, State state, int chars = 0, const QString& message = QString());
    State itemState(int id, bool* known = nullptr) const;

signals:
    // A picture's bytes or typed text arrived (already within the limits; text is valid UTF-8). `id` is the item's number.
    void received(int id, clipitto::phone::PhoneServer::Kind kind, const QByteArray& body);
    void stopped(clipitto::phone::PhoneServer::StopReason reason);

private:
    friend class PhoneConnection;
    struct Item {
        State state = State::Sent;
        int chars = 0;
        QString message;
    };
    enum class Route { None, Page, Upload, Text, Status };

    void onNewConnection();
    void connectionDone(PhoneConnection* connection);
    Route route(const QByteArray& method, const QByteArray& target, QByteArray* query) const;
    bool tokenMatches(const QByteArray& target) const;
    int pendingItems() const;
    QByteArray statusJson(const QByteArray& query) const;

    Config m_config;
    QTcpServer* m_server;
    QString m_token;
    QByteArray m_tokenBytes;
    QDeadlineTimer m_deadline;
    QTimer m_expiry;
    int m_uploads = 0; // accepted
    int m_inflight = 0; // uploads being received
    int m_nextId = 1;
    QHash<int, Item> m_items;
    QList<PhoneConnection*> m_connections;
    int m_refused = 0; // connections turned away (not local / too many), for the debug line at the end
    std::function<bool(const QHostAddress&)> m_peerFilter;
    bool m_running = false;
};

} // namespace clipitto::phone

Q_DECLARE_METATYPE(clipitto::phone::PhoneServer::Kind)
Q_DECLARE_METATYPE(clipitto::phone::PhoneServer::StopReason)
