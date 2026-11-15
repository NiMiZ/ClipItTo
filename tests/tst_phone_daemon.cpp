#include "ControlChannel.h"
#include "Daemon.h"
#include "PhoneNet.h"
#include "PhoneServer.h"
#include "PhoneWindow.h"
#include "SettingsDialog.h"
#include "StallWatchdog.h"
#include "db/Repository.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace clipitto;

namespace {

class FakeControl : public ControlChannel {
public:
    StartResult start(QString*) override { return StartResult::Ok; }
};

int waitMs() { return StallWatchdog::scaled(20000); }

QByteArray encoded(const QImage& image, const char* format)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

// A small picture with some structure (so two of them differ), as a real file would be.
QImage fakePicture(int seed = 0)
{
    QImage image(120, 80, QImage::Format_RGB32);
    image.fill(QColor(200, 220 - seed, 240));
    QPainter p(&image);
    p.setPen(Qt::black);
    p.drawText(10, 40, QStringLiteral("fake %1").arg(seed));
    p.drawLine(0, seed % 80, 119, 79 - seed % 80);
    return image;
}

// Answer of one HTTP request to 127.0.0.1 (the server is in this thread, so everything waits with the event loop).
struct Answer {
    int status = 0;
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

Answer request(quint16 port, const QByteArray& method, const QByteArray& path, const QByteArray& body = {})
{
    QTcpSocket socket;
    QByteArray raw;
    bool closed = false;
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket, [&] { raw += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &socket, [&] { closed = true; });
    QObject::connect(&socket, &QTcpSocket::errorOccurred, &socket, [&] { closed = true; });
    socket.connectToHost(QHostAddress::LocalHost, port);
    (void)QTest::qWaitFor([&] { return socket.state() == QAbstractSocket::ConnectedState || closed; }, waitMs());
    QByteArray head = method + " " + path + " HTTP/1.1\r\nHost: x\r\n";
    if (method == "POST") {
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    socket.write(head + "\r\n" + body);
    (void)QTest::qWaitFor(
        [&] {
            const qsizetype end = raw.indexOf("\r\n\r\n");
            if (end < 0) {
                return closed;
            }
            const qsizetype l = raw.toLower().indexOf("content-length:");
            const qint64 want = l < 0 ? 0 : raw.mid(l + 15, raw.indexOf("\r\n", l) - l - 15).trimmed().toLongLong();
            return raw.size() - (end + 4) >= want || closed;
        },
        waitMs());
    Answer a;
    a.status = raw.mid(9, 3).toInt();
    a.body = raw.mid(raw.indexOf("\r\n\r\n") + 4);
    return a;
}

constexpr const char* kOcrText = "Hello from the picture\nsecond line";

} // namespace

// "Receive from phone" on the daemon's side (roadmap 28): uploads are decoded safely, the picture is kept, its text is read by a
// fake engine and added as a text copy once; the window shows it; panic, quit and closing stop the server. Offscreen; the server
// listens on the loopback only and no real network interface is asked.
class TestPhoneDaemon : public QObject {
    Q_OBJECT

private:
    DaemonOptions options(bool withOcr = true)
    {
        DaemonOptions o;
        o.configPath = m_dir.filePath(QStringLiteral("config.toml"));
        o.databasePath = m_dir.filePath(QStringLiteral("data/history.db"));
        o.debugLogPath = m_dir.filePath(QStringLiteral("debug.log"));
        o.syntheticPaste = [this](PasteKeys) {
            ++m_pastes;
            return true;
        };
        phone::PhoneServer::Config server;
        server.listenOn = QHostAddress::LocalHost;
        o.phoneConfig = server;
        o.phoneInterfaces = [] {
            phone::InterfaceInfo wifi;
            wifi.name = QStringLiteral("wlan0");
            wifi.addresses = {QHostAddress(QStringLiteral("192.168.1.20"))};
            phone::InterfaceInfo docker;
            docker.name = QStringLiteral("docker0");
            docker.addresses = {QHostAddress(QStringLiteral("172.17.0.1"))};
            return QList<phone::InterfaceInfo>{docker, wifi};
        };
        if (withOcr) {
            m_ocr = std::make_shared<FakeOcr>();
            m_ocrCalls = 0;
            m_ocr->recognize = [this](const QByteArray& png, const QStringList&, QString*) -> std::optional<QString> {
                ++m_ocrCalls;
                m_ocrSawPng = png.startsWith("\x89PNG");
                return QString::fromUtf8(kOcrText);
            };
            o.fakeOcr = m_ocr;
        }
        return o;
    }

    static int countImages(Daemon& d) { return int(d.repository()->countImages()); }
    static QStringList texts(Daemon& d)
    {
        QStringList out;
        for (const Entry& e : d.repository()->allEntries()) {
            if (!e.image) {
                out << e.content;
            }
        }
        return out;
    }
    // The item's state as the phone sees it, polled until it is over.
    QJsonObject finalState(phone::PhoneServer& server, int id)
    {
        const QByteArray path = "/" + server.token().toLatin1() + "/status?id=" + QByteArray::number(id);
        QJsonObject state;
        (void)QTest::qWaitFor(
            [&] {
                state = request(server.port(), "GET", path).json();
                const QString s = state.value(QStringLiteral("state")).toString();
                return s == QLatin1String("done") || s == QLatin1String("failed");
            },
            waitMs());
        return state;
    }
    static QByteArray path(phone::PhoneServer& server, const char* what) { return "/" + server.token().toLatin1() + "/" + what; }

    QTemporaryDir m_dir;
    std::shared_ptr<FakeOcr> m_ocr;
    std::atomic<int> m_ocrCalls{0};
    std::atomic<bool> m_ocrSawPng{false};
    int m_pastes = 0;

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }
    void init()
    {
        QDir(m_dir.path()).removeRecursively();
        QDir().mkpath(m_dir.path());
        QApplication::clipboard()->clear(); // what an earlier test left there would be captured by the next daemon
        QTest::qWait(50);
    }

    void nothingListensUntilTheWindowIsOpened()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        QVERIFY(!daemon.phoneServer());
        QVERIFY(!daemon.phoneWindow());
        QCOMPARE(daemon.status().value(QStringLiteral("phone_receive")).toString(), QStringLiteral("off"));
    }

    void windowShowsTheQrCodeTheAddressAndTheRules()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        ui::PhoneWindow* window = daemon.phoneWindow();
        QVERIFY(window);
        QVERIFY(window->isVisible());
        phone::PhoneServer* server = daemon.phoneServer();
        QVERIFY(server && server->isListening());
        QVERIFY(window->hasQr());
        // The real LAN address first (192.168.1.20), the docker one offered as an alternative, the port and token are the server's.
        const QString url = window->urlLabel()->text();
        QCOMPARE(url, QStringLiteral("http://192.168.1.20:%1/%2/").arg(server->port()).arg(server->token()));
        QVERIFY(window->addressBox()->isVisibleTo(window));
        QCOMPARE(window->addressBox()->count(), 2);
        QVERIFY(window->othersLabel()->text().contains(QLatin1String("172.17.0.1")));
        QVERIFY(window->countdownLabel()->text().contains(QRegularExpression(QStringLiteral("[0-5]:\\d\\d"))));
        QVERIFY(window->noticeLabel()->text().contains(QLatin1String("5 minutes")));
        QVERIFY(window->noticeLabel()->text().contains(QLatin1String("same Wi-Fi")));
        QVERIFY(window->noticeLabel()->text().contains(QLatin1String("private networks")));
        QVERIFY(window->stopButton()->isEnabled());
        QVERIFY(window->extendButton()->isEnabled());
        // Choosing the other address changes the text.
        window->addressBox()->setCurrentIndex(1);
        QVERIFY(window->urlLabel()->text().startsWith(QLatin1String("http://172.17.0.1:")));
        QVERIFY(daemon.status().value(QStringLiteral("phone_receive")).toString().startsWith(QLatin1String("on")));
        // Opening it again raises the same window and keeps the session.
        const QString token = server->token();
        daemon.openPhone();
        QCOMPARE(daemon.phoneWindow(), window);
        QCOMPARE(daemon.phoneServer()->token(), token);
    }

    void extendAndStopButtons()
    {
        DaemonOptions o = options();
        o.phoneConfig->sessionMs = 20000;
        o.phoneConfig->extendMs = 100000;
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        ui::PhoneWindow* window = daemon.phoneWindow();
        phone::PhoneServer* server = daemon.phoneServer();
        const qint64 before = server->remainingMs();
        QVERIFY(before <= 20000);
        window->extendButton()->click();
        QVERIFY(server->remainingMs() > 100000);
        QPointer<ui::PhoneWindow> guard(window);
        const quint16 port = server->port();
        const QString url = window->urlLabel()->text();
        QVERIFY(!url.isEmpty());
        window->stopButton()->click();
        QVERIFY(!server->isListening());
        QVERIFY(window->urlLabel()->text().isEmpty()); // the address is not left on the screen
        QVERIFY(!window->hasQr());
        QVERIFY(!window->stopButton()->isEnabled());
        QVERIFY(!window->extendButton()->isEnabled());
        Q_UNUSED(port);
        QCOMPARE(daemon.status().value(QStringLiteral("phone_receive")).toString(), QStringLiteral("off"));
    }

    void expiryEndsTheSessionInTheWindow()
    {
        DaemonOptions o = options();
        o.phoneConfig->sessionMs = 300;
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        ui::PhoneWindow* window = daemon.phoneWindow();
        phone::PhoneServer* server = daemon.phoneServer();
        QTRY_VERIFY_WITH_TIMEOUT(!server->isListening(), waitMs());
        QVERIFY(window->urlLabel()->text().isEmpty());
        QVERIFY(window->countdownLabel()->text().contains(QLatin1String("expired")));
    }

    void aPictureIsKeptAndItsTextBecomesOneTextCopy()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        ui::PhoneWindow* window = daemon.phoneWindow();
        const qint64 before = daemon.repository()->count();

        const Answer up = request(server->port(), "POST", path(*server, "upload"), encoded(fakePicture(), "JPEG"));
        QCOMPARE(up.status, 200);
        const int id = up.json().value(QStringLiteral("id")).toInt();
        const QJsonObject state = finalState(*server, id);
        QCOMPARE(state.value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        QCOMPARE(state.value(QStringLiteral("chars")).toInt(), int(QString::fromUtf8(kOcrText).size()));

        QCOMPARE(countImages(daemon), 1); // the picture is kept
        QVERIFY(texts(daemon).contains(QString::fromUtf8(kOcrText))); // and its text, as a copy
        QCOMPARE(daemon.repository()->count(), before + 2);
        QVERIFY(m_ocrSawPng);

        // The window: thumbnail, status, text, buttons.
        QCOMPARE(window->itemCount(), 1);
        QCOMPARE(window->itemText(id), QString::fromUtf8(kOcrText));
        QVERIFY(window->itemStatus(id).startsWith(QLatin1String("Done: ")));
        QVERIFY(!window->itemThumbnail(id)->pixmap().isNull());
        QVERIFY(window->itemCopyButton(id)->isEnabled());
        QVERIFY(window->itemPasteButton(id)->isEnabled());

        // Nothing reads the picture or records the text a second time (the search index was told the text).
        QTest::qWait(800);
        QCOMPARE(m_ocrCalls.load(), 1);
        QCOMPARE(daemon.repository()->count(), before + 2);
        QCOMPARE(texts(daemon).count(QString::fromUtf8(kOcrText)), 1);

        // Copy text: on the clipboard as an own write, not captured as a new copy.
        window->itemCopyButton(id)->click();
        QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8(kOcrText));
        QTest::qWait(800);
        QCOMPARE(daemon.repository()->count(), before + 2);
        QCOMPARE(texts(daemon).count(QString::fromUtf8(kOcrText)), 1);

        // Paste: the same, and the paste keys go out.
        QApplication::clipboard()->setText(QStringLiteral("something else"));
        QTest::qWait(300);
        const qint64 now = daemon.repository()->count();
        window->itemPasteButton(id)->click();
        QCOMPARE(QApplication::clipboard()->text(), QString::fromUtf8(kOcrText));
        QTRY_VERIFY_WITH_TIMEOUT(m_pastes >= 1, waitMs());
        QTest::qWait(500);
        QCOMPARE(daemon.repository()->count(), now);
    }

    void theSamePictureTwiceIsOneEntry()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        const QByteArray png = encoded(fakePicture(3), "PNG");
        for (int i = 0; i < 2; ++i) {
            const int id = request(server->port(), "POST", path(*server, "upload"), png).json().value(QStringLiteral("id")).toInt();
            QCOMPARE(finalState(*server, id).value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        }
        QCOMPARE(countImages(daemon), 1);
        QCOMPARE(texts(daemon).count(QString::fromUtf8(kOcrText)), 1);
    }

    void typedTextArrivesInTheHistory()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        const QString typed = QStringLiteral("typed on the phone: héllo ✓");
        const int id = request(server->port(), "POST", path(*server, "text"), typed.toUtf8()).json().value(QStringLiteral("id")).toInt();
        const QJsonObject state = finalState(*server, id);
        QCOMPARE(state.value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        QCOMPARE(state.value(QStringLiteral("chars")).toInt(), int(typed.size()));
        QVERIFY(texts(daemon).contains(typed));
        QCOMPARE(countImages(daemon), 0);
        QCOMPARE(daemon.phoneWindow()->itemText(id), typed);
        QCOMPARE(m_ocrCalls.load(), 0);
    }

    void withoutTextRecognitionThePictureIsKeptAndTheReasonIsSaid()
    {
        DaemonOptions o = options();
        o.fakeOcr->unavailable = true;
        o.fakeOcr->reason = QStringLiteral("No text recognition engine is installed (test).");
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        const int id = request(server->port(), "POST", path(*server, "upload"), encoded(fakePicture(), "PNG"))
                           .json().value(QStringLiteral("id")).toInt();
        const QJsonObject state = finalState(*server, id);
        QCOMPARE(state.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QVERIFY(state.value(QStringLiteral("error")).toString().contains(QLatin1String("No text recognition engine is installed (test).")));
        QVERIFY(state.value(QStringLiteral("error")).toString().contains(QLatin1String("kept")));
        QCOMPARE(countImages(daemon), 1);
        QVERIFY(texts(daemon).isEmpty());
        QCOMPARE(m_ocrCalls.load(), 0);
        QVERIFY(daemon.phoneWindow()->itemStatus(id).contains(QLatin1String("No text recognition engine is installed (test).")));
    }

    void anEngineThatFailsAtRunTimeKeepsThePictureToo()
    {
        DaemonOptions o = options();
        o.fakeOcr->recognize = [](const QByteArray&, const QStringList&, QString* error) -> std::optional<QString> {
            *error = QStringLiteral("language data missing (test)");
            return std::nullopt;
        };
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        const int id = request(server->port(), "POST", path(*server, "upload"), encoded(fakePicture(), "PNG"))
                           .json().value(QStringLiteral("id")).toInt();
        const QJsonObject state = finalState(*server, id);
        QCOMPARE(state.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QVERIFY(state.value(QStringLiteral("error")).toString().contains(QLatin1String("language data missing (test)")));
        QCOMPARE(countImages(daemon), 1);
        QVERIFY(texts(daemon).isEmpty());
    }

    void picturesThatCannotBeUsedAreRefusedAndNothingIsStored()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();

        // A BMP whose header claims 20000 x 20000 pixels (400 megapixels) and has no pixels: refused before any is read.
        QByteArray bomb;
        QDataStream out(&bomb, QIODevice::WriteOnly);
        out.setByteOrder(QDataStream::LittleEndian);
        out.writeRawData("BM", 2);
        out << quint32(54) << quint32(0) << quint32(54);
        out << quint32(40) << qint32(20000) << qint32(20000) << quint16(1) << quint16(24) << quint32(0) << quint32(0)
            << qint32(2835) << qint32(2835) << quint32(0) << quint32(0);
        const QList<QByteArray> bad = {
            bomb,
            QByteArray("this is not a picture at all"),
            QByteArray("GIF89a"), // a header and nothing else
            QByteArray("%PDF-1.4 not a picture"),
            QByteArray("<svg xmlns=\"x\"><script>1</script></svg>"),
            QByteArray("\x00\x00\x00\x18" "ftypheic" "\x00\x00\x00\x00mif1heic", 24), // HEIC: no decoder here
            encoded(fakePicture(), "PNG").left(60), // cut off
        };
        for (const QByteArray& data : bad) {
            const int id = request(server->port(), "POST", path(*server, "upload"), data).json().value(QStringLiteral("id")).toInt();
            QVERIFY(id > 0);
            const QJsonObject state = finalState(*server, id);
            QCOMPARE(state.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
            QVERIFY(!state.value(QStringLiteral("error")).toString().isEmpty());
        }
        QCOMPARE(countImages(daemon), 0);
        QVERIFY(texts(daemon).isEmpty());
        QCOMPARE(m_ocrCalls.load(), 0);
    }

    void aPictureIsNeverWrittenToDisk()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        const QDir tmp(QDir::tempPath());
        const int before = int(tmp.entryList(QDir::Files | QDir::NoDotAndDotDot).size());
        const int id = request(server->port(), "POST", path(*server, "upload"), encoded(fakePicture(5), "JPEG")).json().value(QStringLiteral("id")).toInt();
        QCOMPARE(finalState(*server, id).value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        QCOMPARE(int(tmp.entryList(QDir::Files | QDir::NoDotAndDotDot).size()), before);
    }

    void aSessionTakesAtMostThirtyUploads()
    {
        DaemonOptions o = options();
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        QCOMPARE(server->config().maxUploads, 30);
        for (int i = 0; i < 30; ++i) {
            const Answer a = request(server->port(), "POST", path(*server, "text"), QStringLiteral("line %1").arg(i).toUtf8());
            QCOMPARE(a.status, 200);
            QCOMPARE(finalState(*server, a.json().value(QStringLiteral("id")).toInt()).value(QStringLiteral("state")).toString(),
                     QStringLiteral("done"));
        }
        QCOMPARE(request(server->port(), "POST", path(*server, "text"), "one more").status, 429);
        QCOMPARE(daemon.phoneWindow()->itemCount(), 30);
    }

    void panicStopsTheServerAndClosesTheWindow()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        const int id = request(server->port(), "POST", path(*server, "text"), "secret thing").json().value(QStringLiteral("id")).toInt();
        QCOMPARE(finalState(*server, id).value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        QSignalSpy stopped(server, &phone::PhoneServer::stopped);
        daemon.panic();
        QCOMPARE(stopped.count(), 1);
        QCOMPARE(stopped.at(0).at(0).value<phone::PhoneServer::StopReason>(), phone::PhoneServer::StopReason::Panic);
        QVERIFY(!server->isListening());
        QVERIFY(server->token().isEmpty());
        QTRY_VERIFY(!daemon.phoneWindow()); // and what it showed is gone
        QCOMPARE(daemon.repository()->count(), qint64(0));
    }

    void aPictureStillBeingReadWhenPanicComesIsNotStored()
    {
        DaemonOptions o = options();
        std::atomic<bool> inside{false};
        std::atomic<bool> release{false};
        o.fakeOcr->recognize = [&](const QByteArray&, const QStringList&, QString*) -> std::optional<QString> {
            inside = true;
            QTest::qWait(0); // not the GUI thread: just a spin below
            while (!release) {
                QThread::msleep(10);
            }
            return QStringLiteral("late text");
        };
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        (void)request(server->port(), "POST", path(*server, "upload"), encoded(fakePicture(), "PNG"));
        QTRY_VERIFY_WITH_TIMEOUT(inside.load(), waitMs());
        daemon.panic();
        release = true;
        QTest::qWait(800);
        QVERIFY(texts(daemon).isEmpty());
        QCOMPARE(daemon.repository()->count(), qint64(0));
    }

    void quitStopsTheServer()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        QVERIFY(server->isListening());
        emit daemon.control()->quitRequested();
        QVERIFY(!server->isListening());
        QTRY_VERIFY(!daemon.phoneWindow());
    }

    void closingTheWindowStopsTheServer()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        daemon.phoneWindow()->close();
        QVERIFY(!server->isListening());
    }

    void thePhoneRequestOfTheControlChannelOpensTheWindow()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        emit daemon.control()->phoneRequested();
        QVERIFY(daemon.phoneWindow());
        QVERIFY(daemon.phoneServer()->isListening());
    }

    void withoutANetworkTheWindowSaysSoAndNothingListens()
    {
        DaemonOptions o = options();
        o.phoneInterfaces = [] { return QList<phone::InterfaceInfo>{}; };
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        QVERIFY(daemon.phoneWindow());
        QVERIFY(!daemon.phoneServer());
        QVERIFY(!daemon.phoneWindow()->hasQr());
        QVERIFY(daemon.phoneWindow()->statusLabel()->text().contains(QLatin1String("No local network")));
        QVERIFY(!daemon.phoneWindow()->stopButton()->isEnabled());
    }

    void settingsButtonOpensTheWindowOwnedByTheDialog()
    {
        Daemon daemon(options(), std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        auto dialog = std::make_unique<ui::SettingsDialog>(daemon.config(), *daemon.repository(), ui::ShortcutSupport{});
        QVERIFY(!dialog->phoneButton()->isEnabled());
        dialog->setPhone(daemon.phoneSupport());
        QVERIFY(dialog->phoneButton()->isEnabled());
        QCOMPARE(dialog->phoneButton()->text(), QStringLiteral("Receive from phone…"));
        dialog->phoneButton()->click();
        QVERIFY(daemon.phoneWindow());
        QCOMPARE(daemon.phoneWindow()->parentWidget(), dialog.get());
        QVERIFY(daemon.phoneServer()->isListening());
        dialog.reset(); // closing Settings closes the window with it, and the server with the window
        QVERIFY(!daemon.phoneServer()->isListening());
    }

    void nothingFromThePhoneIsLogged()
    {
        DaemonOptions o = options();
        o.verbose = true; // debug lines and the log file
        Daemon daemon(o, std::make_unique<FakeControl>());
        QVERIFY(daemon.start() == Daemon::StartResult::Ok);
        daemon.openPhone();
        phone::PhoneServer* server = daemon.phoneServer();
        const QString token = server->token();
        const QString url = daemon.phoneWindow()->urlLabel()->text();
        const int id = request(server->port(), "POST", path(*server, "text"), "private words 12345").json().value(QStringLiteral("id")).toInt();
        QCOMPARE(finalState(*server, id).value(QStringLiteral("state")).toString(), QStringLiteral("done"));
        daemon.stopPhone(phone::PhoneServer::StopReason::User);
        QFile log(options().debugLogPath);
        if (log.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = log.readAll();
            QVERIFY(!bytes.contains(token.toUtf8()));
            QVERIFY(!bytes.contains("192.168.1.20"));
            QVERIFY(!bytes.contains("private words"));
        }
        Q_UNUSED(url);
    }
};

QTEST_MAIN(TestPhoneDaemon)
#include "tst_phone_daemon.moc"
