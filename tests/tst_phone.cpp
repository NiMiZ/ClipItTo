#include "PhoneNet.h"
#include "PhonePage.h"
#include "PhoneServer.h"
#include "StallWatchdog.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>

#include <memory>
#include <vector>

using namespace clipitto::phone;

namespace {

// A generous wait: loaded CI machines pause for seconds. Nothing here depends on a short wall-clock limit.
int waitMs() { return StallWatchdog::scaled(20000); }

struct Reply {
    bool connected = false;
    bool closed = false;
    QByteArray raw;

    int status() const
    {
        static const QRegularExpression re(QStringLiteral("^HTTP/1\\.1 (\\d+) "));
        const auto m = re.match(QString::fromLatin1(raw.left(32)));
        return m.hasMatch() ? m.captured(1).toInt() : 0;
    }
    QByteArray head() const { return raw.left(qMax<qsizetype>(0, raw.indexOf("\r\n\r\n"))); }
    QByteArray body() const { return raw.mid(raw.indexOf("\r\n\r\n") + 4); }
    bool hasHeader(const QByteArray& name) const { return head().toLower().contains("\r\n" + name.toLower() + ":"); }
    qint64 contentLength() const
    {
        const QByteArray h = head().toLower() + "\r\n";
        const qsizetype i = h.indexOf("content-length:");
        return i < 0 ? -1 : h.mid(i + 15, h.indexOf("\r\n", i) - i - 15).trimmed().toLongLong();
    }
    bool complete() const
    {
        const qsizetype end = raw.indexOf("\r\n\r\n");
        return end >= 0 && contentLength() >= 0 && raw.size() - (end + 4) >= contentLength();
    }
    QJsonObject json() const { return QJsonDocument::fromJson(body()).object(); }
};

// One connection to 127.0.0.1:port; the server runs in this thread, so everything waits with the event loop.
class Client {
public:
    explicit Client(quint16 port)
    {
        QObject::connect(&m_socket, &QTcpSocket::connected, &m_socket, [this] { m_reply.connected = true; });
        QObject::connect(&m_socket, &QTcpSocket::readyRead, &m_socket, [this] { m_reply.raw += m_socket.readAll(); });
        QObject::connect(&m_socket, &QTcpSocket::errorOccurred, &m_socket, [this] { m_reply.closed = true; }); // refused
        QObject::connect(&m_socket, &QTcpSocket::disconnected, &m_socket, [this] {
            if (m_socket.isOpen()) {
                m_reply.raw += m_socket.readAll();
            }
            m_reply.closed = true;
        });
        m_socket.connectToHost(QHostAddress::LocalHost, port);
    }
    bool waitConnected() { return QTest::qWaitFor([this] { return m_reply.connected || m_reply.closed; }, waitMs()) && m_reply.connected; }
    void send(const QByteArray& data) { m_socket.write(data); }
    // The answer once it is complete (or the line closed).
    Reply& waitReply()
    {
        (void)QTest::qWaitFor([this] { return m_reply.complete() || m_reply.closed; }, waitMs());
        return m_reply;
    }
    bool waitClosed() { return QTest::qWaitFor([this] { return m_reply.closed; }, waitMs()); }
    Reply& reply() { return m_reply; }
    void abort() { m_socket.abort(); }

private:
    QTcpSocket m_socket;
    Reply m_reply;
};

PhoneServer::Config smallConfig()
{
    PhoneServer::Config c;
    c.listenOn = QHostAddress::LocalHost;
    return c;
}

Reply get(quint16 port, const QByteArray& path)
{
    Client c(port);
    c.waitConnected();
    c.send("GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n");
    return c.waitReply();
}

Reply post(quint16 port, const QByteArray& path, const QByteArray& body)
{
    Client c(port);
    c.waitConnected();
    c.send("POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    return c.waitReply();
}

} // namespace

class TestPhone : public QObject {
    Q_OBJECT

private slots:
    // ---- pure functions ----
    void localAddressTable_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("local");
        const auto row = [](const char* a, bool local) { QTest::newRow(a) << QString::fromLatin1(a) << local; };
        row("127.0.0.1", true);
        row("127.255.255.254", true);
        row("10.0.0.1", true);
        row("10.255.255.255", true);
        row("172.16.0.1", true);
        row("172.31.255.255", true);
        row("172.15.255.255", false);
        row("172.32.0.1", false);
        row("192.168.0.1", true);
        row("192.168.255.255", true);
        row("192.167.0.1", false);
        row("192.169.0.1", false);
        row("169.254.1.1", true);
        row("169.253.1.1", false);
        row("8.8.8.8", false);
        row("1.1.1.1", false);
        row("100.64.0.1", false); // carrier-grade NAT / Tailscale: not the local network
        row("0.0.0.0", false);
        row("224.0.0.1", false);
        row("255.255.255.255", false);
        row("11.0.0.1", false);
        row("::1", true);
        row("::", false);
        row("fc00::1", true);
        row("fd12:3456::1", true);
        row("fe00::1", false);
        row("fe80::1", true);
        row("febf::1", true);
        row("fec0::1", false);
        row("2001:4860:4860::8888", false);
        row("ff02::1", false);
        row("::ffff:192.168.1.5", true); // IPv4-mapped: judged as the IPv4 address
        row("::ffff:127.0.0.1", true);
        row("::ffff:8.8.8.8", false);
        row("::ffff:172.32.0.1", false);
        row("::ffff:10.1.2.3", true);
        row("::192.168.1.5", false); // IPv4-compatible (obsolete) is not mapped
    }
    void localAddressTable()
    {
        QFETCH(QString, address);
        QFETCH(bool, local);
        QVERIFY2(!QHostAddress(address).isNull(), qPrintable(address));
        QCOMPARE(isLocalAddress(QHostAddress(address)), local);
    }
    void nullAddressIsNotLocal() { QVERIFY(!isLocalAddress(QHostAddress())); }
    void lanIpv4Only()
    {
        QVERIFY(isLanIpv4(QHostAddress(QStringLiteral("192.168.1.2"))));
        QVERIFY(isLanIpv4(QHostAddress(QStringLiteral("169.254.3.4"))));
        QVERIFY(!isLanIpv4(QHostAddress(QStringLiteral("127.0.0.1"))));
        QVERIFY(!isLanIpv4(QHostAddress(QStringLiteral("8.8.8.8"))));
        QVERIFY(!isLanIpv4(QHostAddress(QStringLiteral("fe80::1"))));
    }

    void tokenIsRandomHex()
    {
        const QString a = newToken();
        const QString b = newToken();
        QCOMPARE(a.size(), 32);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(a).hasMatch());
        QVERIFY(a != b);
        QVERIFY(newToken() != newToken());
    }
    void constantTimeCompare()
    {
        QVERIFY(constantTimeEqual("abc", "abc"));
        QVERIFY(constantTimeEqual("", ""));
        QVERIFY(!constantTimeEqual("abc", "abd"));
        QVERIFY(!constantTimeEqual("abc", "abcd"));
        QVERIFY(!constantTimeEqual("abcd", "abc"));
        QVERIFY(!constantTimeEqual("", "a"));
        QVERIFY(!constantTimeEqual(QByteArray("\0a", 2), QByteArray("\0b", 2)));
        QVERIFY(constantTimeEqual(QByteArray("\0\xff", 2), QByteArray("\0\xff", 2)));
        const QString t = newToken();
        QVERIFY(constantTimeEqual(t.toLatin1(), t.toLatin1()));
        QVERIFY(!constantTimeEqual(t.toLatin1(), t.left(31).toLatin1()));
    }
    void urlBuilding()
    {
        QCOMPARE(buildUrl(QStringLiteral("192.168.1.20"), 40123, QStringLiteral("abc")), QStringLiteral("http://192.168.1.20:40123/abc/"));
        QCOMPARE(buildUrl(QStringLiteral("10.0.0.2"), 80, QStringLiteral("t")), QStringLiteral("http://10.0.0.2:80/t/"));
    }

    void candidatesPickRealInterfacesFirst()
    {
        const auto iface = [](const char* name, const char* human, std::initializer_list<const char*> addresses, bool up = true,
                              bool loopback = false, bool p2p = false) {
            InterfaceInfo i;
            i.name = QString::fromLatin1(name);
            i.humanName = QString::fromLatin1(human);
            i.up = up;
            i.loopback = loopback;
            i.pointToPoint = p2p;
            for (const char* a : addresses) {
                i.addresses.append(QHostAddress(QString::fromLatin1(a)));
            }
            return i;
        };
        const QList<InterfaceInfo> list = {
            iface("lo", "lo", {"127.0.0.1", "::1"}, true, true),
            iface("docker0", "docker0", {"172.17.0.1"}),
            iface("br-1a2b3c", "br-1a2b3c", {"172.18.0.1"}),
            iface("veth9f", "veth9f", {"169.254.5.5"}),
            iface("tun0", "tun0", {"10.8.0.2"}, true, false, true),
            iface("wlan0", "wlan0", {"fe80::1", "10.0.0.7"}),
            iface("eth0", "eth0", {"192.168.1.20"}),
            iface("eth1", "eth1", {"192.168.9.9"}, /*up=*/false),
            iface("enp3s0", "Ethernet", {"8.8.4.4"}), // public: not offered
        };
        const QList<Candidate> found = lanCandidates(list);
        QVERIFY(found.size() >= 2);
        QCOMPARE(found.at(0).address, QStringLiteral("192.168.1.20")); // 192.168 before 10
        QCOMPARE(found.at(1).address, QStringLiteral("10.0.0.7"));
        QVERIFY(!found.at(0).likelyVirtual);
        QVERIFY(!found.at(1).likelyVirtual);
        QStringList all;
        for (const Candidate& c : found) {
            all << c.address;
        }
        QVERIFY(!all.contains(QStringLiteral("127.0.0.1")));
        QVERIFY(!all.contains(QStringLiteral("192.168.9.9"))); // down
        QVERIFY(!all.contains(QStringLiteral("8.8.4.4")));
        QVERIFY(all.contains(QStringLiteral("172.17.0.1"))); // still listed, last
        for (int i = 2; i < found.size(); ++i) {
            QVERIFY2(found.at(i).likelyVirtual, qPrintable(found.at(i).address));
        }
    }
    void virtualNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("isVirtual");
        const auto row = [](const char* n, bool v) { QTest::newRow(n) << QString::fromLatin1(n) << v; };
        row("docker0", true);
        row("vEthernet (WSL (Hyper-V firewall))", true);
        row("VMware Network Adapter VMnet8", true);
        row("VirtualBox Host-Only Network", true);
        row("virbr0", true);
        row("tailscale0", true);
        row("utun3", true);
        row("wg0", true);
        row("Wi-Fi", false);
        row("Ethernet", false);
        row("wlan0", false);
        row("wlp2s0", false);
        row("enp3s0", false);
        row("eth0", false);
        row("en0", false);
    }
    void virtualNames()
    {
        QFETCH(QString, name);
        QFETCH(bool, isVirtual);
        InterfaceInfo i;
        i.name = name;
        i.humanName = name;
        QCOMPARE(looksVirtual(i), isVirtual);
    }
    void noCandidatesWithoutNetwork()
    {
        InterfaceInfo lo;
        lo.name = QStringLiteral("lo");
        lo.loopback = true;
        lo.addresses = {QHostAddress(QStringLiteral("127.0.0.1"))};
        QVERIFY(lanCandidates({lo}).isEmpty());
        QVERIFY(lanCandidates({}).isEmpty());
    }

    // ---- the page ----
    void pageSendsNothingAnywhereElse()
    {
        const QByteArray page = pageHtml();
        QVERIFY(page.size() > 2000);
        QVERIFY(!page.contains("http://"));
        QVERIFY(!page.contains("https://"));
        QVERIFY(!page.contains("//cdn")); // protocol-relative addresses
        QVERIFY(!page.contains("<link"));
        QVERIFY(!page.contains("@import"));
        QVERIFY(!page.contains("url("));
        QVERIFY(!page.contains("src=\"")); // the only src is set from script, to a blob
        QVERIFY(!page.contains("<iframe"));
        QVERIFY(!page.contains("XMLHttpRequest"));
        QVERIFY(!page.contains("WebSocket"));
        QVERIFY(!page.contains("sendBeacon"));
        // Every request the script makes is relative to the page.
        const QRegularExpression fetchCall(QStringLiteral("fetch\\(([^,)]*)"));
        auto it = fetchCall.globalMatch(QString::fromUtf8(page));
        int calls = 0;
        while (it.hasNext()) {
            const QString arg = it.next().captured(1).trimmed();
            QVERIFY2(!arg.startsWith(QLatin1Char('/')) && !arg.contains(QStringLiteral("//")), qPrintable(arg));
            ++calls;
        }
        QVERIFY(calls >= 2);
    }
    void pageHasTheInputsAndTheConversion()
    {
        const QByteArray page = pageHtml();
        QVERIFY(page.contains("capture=\"environment\""));
        QVERIFY(page.contains("accept=\"image/*\""));
        QVERIFY(page.contains("multiple"));
        QVERIFY(page.contains("Take a picture"));
        QVERIFY(page.contains("Choose from the gallery"));
        QVERIFY(page.contains("Send text"));
        QVERIFY(page.contains("createImageBitmap"));
        QVERIFY(page.contains("imageOrientation:'from-image'"));
        QVERIFY(page.contains("2400"));
        QVERIFY(page.contains("toBlob"));
        QVERIFY(page.contains("image/jpeg"));
        QVERIFY(page.contains("prefers-color-scheme:dark"));
        QVERIFY(page.contains("viewport"));
        QVERIFY(page.contains("Reading the text"));
    }

    // ---- the server ----
    void servesThePageOnlyWithTheToken()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        QVERIFY(server.isListening());
        const quint16 port = server.port();
        QVERIFY(port != 0);
        const QByteArray token = server.token().toLatin1();
        QCOMPARE(token.size(), 32);

        const Reply page = get(port, "/" + token + "/");
        QCOMPARE(page.status(), 200);
        QCOMPARE(page.body(), pageHtml());
        QVERIFY(page.hasHeader("Content-Security-Policy"));
        QVERIFY(page.hasHeader("Cache-Control"));
        QVERIFY(!page.hasHeader("Access-Control-Allow-Origin"));
        QVERIFY(!page.hasHeader("Set-Cookie"));
        QVERIFY(page.head().toLower().contains("connection: close"));

        const QList<QByteArray> notFound = {
            "/", "/index.html", "/" + token, "/" + token + "//", "/" + token + "/x", "/" + token + "/upload", "/" + token + "/text",
            "/" + token.left(31) + "/", "/" + token + "0/", "/" + QByteArray(32, 'a') + "/", "/" + token.toUpper() + "/",
            "/%2F" + token + "/", "/../" + token + "/", "/status", "/" + token + "/../" + token + "/x",
        };
        for (const QByteArray& path : notFound) {
            const Reply r = get(port, path);
            QVERIFY2(r.status() == 404, path.constData());
            QCOMPARE(r.contentLength(), qint64(0)); // bare: no body, no detail
            QVERIFY2(!r.hasHeader("Content-Type"), path.constData());
            QVERIFY(!r.hasHeader("Server"));
        }
        // The page address with a query is the page; nothing is listed anywhere.
        QCOMPARE(get(port, "/" + token + "/?x=1").status(), 200);
    }
    void wrongMethodsAreNotFound()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        for (const char* method : {"HEAD", "OPTIONS", "PUT", "DELETE", "PATCH", "TRACE", "CONNECT", "get"}) {
            Client c(server.port());
            QVERIFY(c.waitConnected());
            c.send(QByteArray(method) + " /" + token + "/ HTTP/1.1\r\nHost: x\r\n\r\n");
            QCOMPARE(c.waitReply().status(), 404);
        }
        QCOMPARE(post(server.port(), "/" + token + "/", "x").status(), 404); // POST to the page
        {
            Client c(server.port());
            QVERIFY(c.waitConnected());
            c.send("GET /" + token + "/upload HTTP/1.1\r\n\r\n");
            QCOMPARE(c.waitReply().status(), 404);
        }
        QCOMPARE(post(server.port(), "/" + token + "/status", "x").status(), 404);
    }
    void malformedRequests()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        const QList<QByteArray> bad = {
            "GARBAGE\r\n\r\n",
            "GET /" + token + "/\r\n\r\n",
            "GET /" + token + "/ HTTP/2.0\r\n\r\n",
            "GET /" + token + "/ HTTP/1.1\r\nno colon here\r\n\r\n",
            "GET x HTTP/1.1\r\n\r\n",
            "POST /" + token + "/upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
            "POST /" + token + "/upload HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc",
            "POST /" + token + "/upload HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
            "POST /" + token + "/upload HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
            "POST /" + token + "/upload HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
            "GET /" + token + "/ HTTP/1.1\r\nContent-Length: 4\r\n\r\nabcd",
        };
        for (const QByteArray& request : bad) {
            Client c(server.port());
            QVERIFY(c.waitConnected());
            c.send(request);
            const int status = c.waitReply().status();
            QVERIFY2(status == 400, request.left(60).constData());
        }
        Client c(server.port());
        QVERIFY(c.waitConnected());
        c.send("POST /" + token + "/upload HTTP/1.1\r\n\r\n");
        QCOMPARE(c.waitReply().status(), 411);
    }
    void headerLimit()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        {
            Client c(server.port());
            QVERIFY(c.waitConnected());
            c.send("GET /" + token + "/ HTTP/1.1\r\nX-Pad: " + QByteArray(9000, 'a') + "\r\n\r\n");
            QCOMPARE(c.waitReply().status(), 431);
        }
        {
            // Never ends: the limit applies to what has been read without a blank line.
            Client c(server.port());
            QVERIFY(c.waitConnected());
            c.send("GET /" + token + "/ HTTP/1.1\r\nX-Pad: " + QByteArray(20000, 'b'));
            QCOMPARE(c.waitReply().status(), 431);
        }
        {
            // Just under the limit is fine.
            Client c(server.port());
            QVERIFY(c.waitConnected());
            c.send("GET /" + token + "/ HTTP/1.1\r\nX-Pad: " + QByteArray(7000, 'a') + "\r\n\r\n");
            QCOMPARE(c.waitReply().status(), 200);
        }
    }
    void bodyLimits()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const quint16 port = server.port();
        const QByteArray token = server.token().toLatin1();
        {
            Client c(port);
            QVERIFY(c.waitConnected());
            c.send("POST /" + token + "/upload HTTP/1.1\r\nContent-Length: " + QByteArray::number(20 * 1024 * 1024 + 1) + "\r\n\r\n");
            QCOMPARE(c.waitReply().status(), 413); // answered before any of the body is sent
        }
        {
            Client c(port);
            QVERIFY(c.waitConnected());
            c.send("POST /" + token + "/text HTTP/1.1\r\nContent-Length: " + QByteArray::number(100 * 1024 + 1) + "\r\n\r\n");
            QCOMPARE(c.waitReply().status(), 413);
        }
        {
            // The wrong token is a 404 even for a huge body: nothing is learned.
            Client c(port);
            QVERIFY(c.waitConnected());
            c.send("POST /" + QByteArray(32, 'a') + "/upload HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n");
            QCOMPARE(c.waitReply().status(), 404);
        }
        QCOMPARE(server.uploadsAccepted(), 0);
        // Exactly at the limits is accepted (text 100 KB of ASCII).
        QSignalSpy spy(&server, &PhoneServer::received);
        QCOMPARE(post(port, "/" + token + "/text", QByteArray(100 * 1024, 'x')).status(), 200);
        QCOMPARE(spy.count(), 1);
    }
    void uploadAndStatus()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const quint16 port = server.port();
        const QByteArray token = server.token().toLatin1();
        QSignalSpy spy(&server, &PhoneServer::received);

        const QByteArray bytes("not really a picture \x01\x02\xff", 24);
        const Reply r = post(port, "/" + token + "/upload", bytes);
        QCOMPARE(r.status(), 200);
        QCOMPARE(r.json().value(QStringLiteral("id")).toInt(), 1);
        QVERIFY(!r.hasHeader("Access-Control-Allow-Origin"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 1);
        QCOMPARE(spy.at(0).at(1).value<PhoneServer::Kind>(), PhoneServer::Kind::Picture);
        QCOMPARE(spy.at(0).at(2).toByteArray(), bytes);

        const auto state = [&](int id) { return get(port, "/" + token + "/status?id=" + QByteArray::number(id)).json(); };
        QCOMPARE(state(1).value(QStringLiteral("state")).toString(), QStringLiteral("sent"));
        server.setItemState(1, PhoneServer::State::Reading);
        QCOMPARE(state(1).value(QStringLiteral("state")).toString(), QStringLiteral("reading"));
        server.setItemState(1, PhoneServer::State::Done, 42);
        QCOMPARE(state(1).value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        QCOMPARE(state(1).value(QStringLiteral("chars")).toInt(), 42);
        server.setItemState(1, PhoneServer::State::Failed, 0, QStringLiteral("no way"));
        QCOMPARE(state(1).value(QStringLiteral("error")).toString(), QStringLiteral("no way"));
        QCOMPARE(state(99).value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
        const QJsonObject ready = get(port, "/" + token + "/status").json();
        QCOMPARE(ready.value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
        QVERIFY(ready.value(QStringLiteral("seconds")).toInt() > 0);
    }
    void typedText()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const quint16 port = server.port();
        const QByteArray token = server.token().toLatin1();
        QSignalSpy spy(&server, &PhoneServer::received);
        const QByteArray text = QStringLiteral("héllo wörld ✓").toUtf8();
        QCOMPARE(post(port, "/" + token + "/text", text).status(), 200);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(1).value<PhoneServer::Kind>(), PhoneServer::Kind::Text);
        QCOMPARE(spy.at(0).at(2).toByteArray(), text);
        QCOMPARE(post(port, "/" + token + "/text", QByteArray("\xff\xfe\xfd", 3)).status(), 400); // not UTF-8
        QCOMPARE(post(port, "/" + token + "/text", "   \n  ").status(), 400); // nothing to keep
        QCOMPARE(spy.count(), 1);
    }
    void expectContinue()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        QSignalSpy spy(&server, &PhoneServer::received);
        Client c(server.port());
        QVERIFY(c.waitConnected());
        c.send("POST /" + token + "/text HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n");
        QVERIFY(QTest::qWaitFor([&] { return c.reply().raw.startsWith("HTTP/1.1 100 Continue"); }, waitMs()));
        c.send("hello");
        QVERIFY(QTest::qWaitFor([&] { return c.reply().raw.contains("HTTP/1.1 200 OK"); }, waitMs()));
        QCOMPARE(spy.count(), 1);
    }
    void bodyInPiecesAndSplitHeaders()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        QSignalSpy spy(&server, &PhoneServer::received);
        Client c(server.port());
        QVERIFY(c.waitConnected());
        const QByteArray head = "POST /" + token + "/upload HTTP/1.1\r\nContent-Le";
        c.send(head);
        QTest::qWait(30);
        c.send("ngth: 10\r\n\r\n01234");
        QTest::qWait(30);
        QCOMPARE(spy.count(), 0);
        c.send("56789");
        QCOMPARE(c.waitReply().status(), 200);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(2).toByteArray(), QByteArray("0123456789"));
    }
    void bytesAfterTheBodyAreIgnored()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        QSignalSpy spy(&server, &PhoneServer::received);
        Client c(server.port());
        QVERIFY(c.waitConnected());
        c.send("POST /" + token + "/upload HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /" + token + "/ HTTP/1.1\r\n\r\n");
        QCOMPARE(c.waitReply().status(), 200);
        QVERIFY(c.waitClosed());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(2).toByteArray(), QByteArray("abc"));
    }
    void uploadLimitPerSession()
    {
        auto config = smallConfig();
        config.maxUploads = 3;
        PhoneServer server(config);
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        for (int i = 1; i <= 3; ++i) {
            const Reply r = post(server.port(), "/" + token + "/text", "t");
            QCOMPARE(r.status(), 200);
            server.setItemState(i, PhoneServer::State::Done, 1);
        }
        QCOMPARE(server.uploadsAccepted(), 3);
        QCOMPARE(post(server.port(), "/" + token + "/text", "t").status(), 429);
        QCOMPARE(post(server.port(), "/" + token + "/upload", "t").status(), 429);
        // The page and the status still work.
        QCOMPARE(get(server.port(), "/" + token + "/").status(), 200);
        // Extending does not reset the count.
        QVERIFY(server.extend());
        QCOMPARE(post(server.port(), "/" + token + "/text", "t").status(), 429);
    }
    void busyWhenTooManyAreStillBeingRead()
    {
        auto config = smallConfig();
        config.maxPending = 2;
        PhoneServer server(config);
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        QCOMPARE(post(server.port(), "/" + token + "/upload", "a").status(), 200);
        QCOMPARE(post(server.port(), "/" + token + "/upload", "b").status(), 200);
        QCOMPARE(post(server.port(), "/" + token + "/upload", "c").status(), 503);
        server.setItemState(1, PhoneServer::State::Done, 0);
        QCOMPARE(post(server.port(), "/" + token + "/upload", "c").status(), 200);
    }
    void idleConnectionIsClosed()
    {
        auto config = smallConfig();
        config.idleMs = 300;
        PhoneServer server(config);
        QVERIFY(server.start());
        Client c(server.port());
        QVERIFY(c.waitConnected());
        QVERIFY(c.waitClosed());
        QVERIFY(c.reply().raw.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 0, waitMs());
    }
    void slowSenderIsClosedByIdleTimeout()
    {
        auto config = smallConfig();
        config.idleMs = 300;
        PhoneServer server(config);
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        Client c(server.port());
        QVERIFY(c.waitConnected());
        c.send("POST /" + token + "/upload HTTP/1.1\r\nContent-Length: 1000\r\n\r\nabc"); // and then nothing
        QVERIFY(c.waitClosed());
        QCOMPARE(server.uploadsAccepted(), 0);
    }
    void connectionLifetimeIsLimited()
    {
        auto config = smallConfig();
        config.idleMs = 5000;
        config.maxConnectionMs = 400;
        PhoneServer server(config);
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        Client c(server.port());
        QVERIFY(c.waitConnected());
        c.send("POST /" + token + "/upload HTTP/1.1\r\nContent-Length: 1000\r\n\r\n");
        for (int i = 0; i < 3; ++i) {
            QTest::qWait(100);
            c.send("x"); // a trickle keeps it from being idle
        }
        QVERIFY(c.waitClosed());
    }
    void atMostFourConnections()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        std::vector<std::unique_ptr<Client>> held;
        for (int i = 0; i < 4; ++i) {
            held.push_back(std::make_unique<Client>(server.port()));
            QVERIFY(held.back()->waitConnected());
        }
        QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 4, waitMs());
        Client fifth(server.port());
        // Accepted by the system, closed by the server at once, unanswered.
        QVERIFY(fifth.waitClosed());
        QVERIFY(fifth.reply().raw.isEmpty());
        QCOMPARE(server.activeConnections(), 4);
        // Room again when one goes.
        held.front()->abort();
        QTRY_COMPARE_WITH_TIMEOUT(server.activeConnections(), 3, waitMs());
        const QByteArray token = server.token().toLatin1();
        QCOMPARE(get(server.port(), "/" + token + "/").status(), 200);
    }
    void strangersAreDroppedUnanswered()
    {
        PhoneServer server(smallConfig());
        QVERIFY(server.start());
        const QByteArray token = server.token().toLatin1();
        server.setPeerFilter([](const QHostAddress& peer) { return peer != QHostAddress(QHostAddress::LocalHost); });
        Client c(server.port());
        QVERIFY(c.waitConnected());
        c.send("GET /" + token + "/ HTTP/1.1\r\n\r\n");
        QVERIFY(c.waitClosed());
        QVERIFY(c.reply().raw.isEmpty());
        server.setPeerFilter({});
        server.setPeerFilter(&isLocalAddress);
        QCOMPARE(get(server.port(), "/" + token + "/").status(), 200);
    }
    void listensOnTheLocalNetworkOnlyByDefault()
    {
        // Not on IPv6 and not on a named interface: the address list the window shows are the IPv4 ones.
        PhoneServer::Config config;
        QCOMPARE(config.listenOn, QHostAddress(QHostAddress::AnyIPv4));
        QCOMPARE(config.maxConnections, 4);
        QCOMPARE(config.idleMs, 10000);
        QCOMPARE(config.maxHeaderBytes, 8192);
        QCOMPARE(config.maxBodyBytes, qint64(20) * 1024 * 1024);
        QCOMPARE(config.maxTextBytes, qint64(100) * 1024);
        QCOMPARE(config.maxUploads, 30);
        QCOMPARE(config.sessionMs, 5 * 60 * 1000);
    }
    void expiryClosesTheServer()
    {
        auto config = smallConfig();
        config.sessionMs = 300;
        PhoneServer server(config);
        QSignalSpy stopped(&server, &PhoneServer::stopped);
        QVERIFY(server.start());
        const quint16 port = server.port();
        const QByteArray token = server.token().toLatin1();
        QVERIFY(server.remainingMs() > 0);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, waitMs());
        QCOMPARE(stopped.at(0).at(0).value<PhoneServer::StopReason>(), PhoneServer::StopReason::Expired);
        QVERIFY(!server.isListening());
        QVERIFY(server.token().isEmpty());
        QCOMPARE(server.remainingMs(), qint64(0));
        Client c(port);
        QVERIFY(!c.waitConnected()); // refused
        QVERIFY(!server.extend()); // nothing to extend
    }
    void extendAddsTime()
    {
        auto config = smallConfig();
        config.sessionMs = 5000;
        config.extendMs = 20000;
        config.maxSessionMs = 40000;
        PhoneServer server(config);
        QSignalSpy stopped(&server, &PhoneServer::stopped);
        QVERIFY(server.start());
        QVERIFY(server.remainingMs() <= 5000);
        QVERIFY(server.extend());
        QVERIFY(server.remainingMs() > 20000);
        QVERIFY(server.extend());
        QVERIFY(server.remainingMs() > 30000);
        QVERIFY(server.extend());
        QVERIFY(server.remainingMs() <= 40000); // capped
        QCOMPARE(stopped.count(), 0);
    }
    void extendKeepsAServerAliveBeyondTheFirstDeadline()
    {
        auto config = smallConfig();
        config.sessionMs = 400;
        config.extendMs = 30000;
        PhoneServer server(config);
        QSignalSpy stopped(&server, &PhoneServer::stopped);
        QVERIFY(server.start());
        QVERIFY(server.extend());
        QTest::qWait(900);
        QCOMPARE(stopped.count(), 0);
        QVERIFY(server.isListening());
    }
    void stopClosesEverything()
    {
        PhoneServer server(smallConfig());
        QSignalSpy stopped(&server, &PhoneServer::stopped);
        QVERIFY(server.start());
        const quint16 port = server.port();
        const QByteArray token = server.token().toLatin1();
        Client open(port);
        QVERIFY(open.waitConnected());
        server.stop(PhoneServer::StopReason::User);
        QCOMPARE(stopped.count(), 1);
        QCOMPARE(stopped.at(0).at(0).value<PhoneServer::StopReason>(), PhoneServer::StopReason::User);
        QVERIFY(open.waitClosed()); // open connections are cut
        Client late(port);
        QVERIFY(!late.waitConnected());
        QVERIFY(server.token().isEmpty());
        server.stop(); // again: nothing
        QCOMPARE(stopped.count(), 1);
        // Started again: a new token and a new port.
        QVERIFY(server.start());
        QVERIFY(server.token().toLatin1() != token);
        QCOMPARE(get(server.port(), "/" + token + "/").status(), 404); // the old address does not work
        QCOMPARE(get(server.port(), "/" + server.token().toLatin1() + "/").status(), 200);
    }
    void destroyedWithOpenConnections()
    {
        auto* server = new PhoneServer(smallConfig());
        QVERIFY(server->start());
        Client a(server->port());
        QVERIFY(a.waitConnected());
        Client b(server->port());
        QVERIFY(b.waitConnected());
        delete server;
        QVERIFY(a.waitClosed());
        QVERIFY(b.waitClosed());
    }
    void startFailsWhenThePortIsTaken()
    {
        PhoneServer first(smallConfig());
        QVERIFY(first.start());
        auto config = smallConfig();
        config.port = first.port();
        PhoneServer second(config);
        QString error;
        QVERIFY(!second.start(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!second.isListening());
    }
};

QTEST_MAIN(TestPhone)
#include "tst_phone.moc"
