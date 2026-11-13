#include "ScreenLockWatcher.h"
#include "ScreenLockWatcher_dbus.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QSignalSpy>
#include <QTest>

using clipitto::DbusScreenLockWatcher;

namespace {

constexpr char kLogin[] = "org.freedesktop.login1";
constexpr char kLoginPath[] = "/org/freedesktop/login1";
constexpr char kManager[] = "org.freedesktop.login1.Manager";
constexpr char kSession[] = "org.freedesktop.login1.Session";
constexpr char kMySession[] = "/org/freedesktop/login1/session/_1test";
constexpr char kOtherSession[] = "/org/freedesktop/login1/session/_2other";

// The part of logind's Manager that the watcher calls: which session is this process in.
class FakeManager : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Manager")

public slots:
    Q_SCRIPTABLE QDBusObjectPath GetSessionByPID(uint) { return QDBusObjectPath(QString::fromLatin1(kMySession)); }
};

} // namespace

// The Linux watcher against a private session bus (ctest runs this under dbus-run-session) that plays logind and the
// screensavers: the same names, paths and signals, no real login session involved. The "system bus" is the same
// private bus. Without such a bus the test skips itself.
class TestScreenLock : public QObject {
    Q_OBJECT

private:
    static void emitSignal(const char* path, const char* interface, const char* name, const QVariantList& args = {})
    {
        QDBusMessage m = QDBusMessage::createSignal(QString::fromLatin1(path), QString::fromLatin1(interface),
                                                    QString::fromLatin1(name));
        m.setArguments(args);
        QVERIFY(QDBusConnection::sessionBus().send(m));
    }

    static QStringList collect(QSignalSpy& spy)
    {
        QStringList out;
        for (const QList<QVariant>& args : spy) {
            out << args.at(0).toString();
        }
        return out;
    }

    // Lets queued bus traffic arrive; "nothing came" cannot be waited for, so a short settle is all that is possible.
    static void settle() { QTest::qWait(200); }

    FakeManager m_manager;

private slots:
    void initTestCase()
    {
        if (!QDBusConnection::sessionBus().isConnected()) {
            QSKIP("no session bus (run under dbus-run-session)");
        }
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService(QString::fromLatin1(kLogin)));
        QVERIFY(bus.registerService(QStringLiteral("org.gnome.ScreenSaver")));
        QVERIFY(bus.registerService(QStringLiteral("org.freedesktop.ScreenSaver")));
        QVERIFY(bus.registerObject(QString::fromLatin1(kLoginPath), &m_manager, QDBusConnection::ExportScriptableSlots));
    }

    void cleanupTestCase()
    {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QString::fromLatin1(kLoginPath));
        bus.unregisterService(QString::fromLatin1(kLogin));
        bus.unregisterService(QStringLiteral("org.gnome.ScreenSaver"));
        bus.unregisterService(QStringLiteral("org.freedesktop.ScreenSaver"));
    }

    void theSessionLockSignalOfTheUsersOwnSessionIsReportedAndOtherSessionsAreNot()
    {
        DbusScreenLockWatcher w(QDBusConnection::sessionBus(), QDBusConnection::sessionBus());
        QSignalSpy spy(&w, &clipitto::ScreenLockWatcher::locked);
        QVERIFY(w.start());
        QVERIFY(w.isWatching());
        // The session is found asynchronously (GetSessionByPID): wait until its Lock signal is wired up.
        QTRY_VERIFY_WITH_TIMEOUT((emitSignal(kMySession, kSession, "Lock"), spy.size() >= 1), 5000);
        QCOMPARE(collect(spy).first(), QStringLiteral("screen lock"));
        spy.clear();
        settle();
        spy.clear();

        emitSignal(kOtherSession, kSession, "Lock"); // somebody else's session
        emitSignal(kMySession, kSession, "Unlock"); // unlocking is ignored
        settle();
        QCOMPARE(spy.size(), 0);
    }

    void prepareForSleepReportsOnlyTheStart()
    {
        DbusScreenLockWatcher w(QDBusConnection::sessionBus(), QDBusConnection::sessionBus(), QString::fromLatin1(kMySession));
        QSignalSpy spy(&w, &clipitto::ScreenLockWatcher::locked);
        QVERIFY(w.start());
        emitSignal(kLoginPath, kManager, "PrepareForSleep", {false}); // waking up: ignored
        settle();
        QCOMPARE(spy.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT((emitSignal(kLoginPath, kManager, "PrepareForSleep", {true}), spy.size() >= 1), 5000);
        QCOMPARE(collect(spy).first(), QStringLiteral("sleep"));
    }

    void screensaversReportOnlyBecomingActive_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("interface");
        QTest::newRow("gnome") << QStringLiteral("/org/gnome/ScreenSaver") << QStringLiteral("org.gnome.ScreenSaver");
        QTest::newRow("freedesktop") << QStringLiteral("/org/freedesktop/ScreenSaver")
                                     << QStringLiteral("org.freedesktop.ScreenSaver");
        QTest::newRow("kde old path") << QStringLiteral("/ScreenSaver") << QStringLiteral("org.freedesktop.ScreenSaver");
    }

    void screensaversReportOnlyBecomingActive()
    {
        QFETCH(QString, path);
        QFETCH(QString, interface);
        DbusScreenLockWatcher w(QDBusConnection::sessionBus(), QDBusConnection::sessionBus(), QString::fromLatin1(kMySession));
        QSignalSpy spy(&w, &clipitto::ScreenLockWatcher::locked);
        QVERIFY(w.start());
        emitSignal(path.toLatin1().constData(), interface.toLatin1().constData(), "ActiveChanged", {false});
        settle();
        QCOMPARE(spy.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(
            (emitSignal(path.toLatin1().constData(), interface.toLatin1().constData(), "ActiveChanged", {true}),
             spy.size() >= 1),
            5000);
    }

    void nothingIsReportedAfterStop()
    {
        DbusScreenLockWatcher w(QDBusConnection::sessionBus(), QDBusConnection::sessionBus(), QString::fromLatin1(kMySession));
        QSignalSpy spy(&w, &clipitto::ScreenLockWatcher::locked);
        QVERIFY(w.start());
        w.stop();
        QVERIFY(!w.isWatching());
        emitSignal(kMySession, kSession, "Lock");
        emitSignal(kLoginPath, kManager, "PrepareForSleep", {true});
        emitSignal("/org/gnome/ScreenSaver", "org.gnome.ScreenSaver", "ActiveChanged", {true});
        settle();
        QCOMPARE(spy.size(), 0);
        QVERIFY(w.start()); // and it can be started again
        QVERIFY(w.isWatching());
    }

    void withoutAnyBusThereIsAnExplicitUnsupportedResult()
    {
        QDBusConnection nobody = QDBusConnection::connectToBus(QStringLiteral("unix:path=/nonexistent/clipitto-test-bus"),
                                                               QStringLiteral("clipitto-no-bus"));
        QVERIFY(!nobody.isConnected());
        DbusScreenLockWatcher w(nobody, nobody);
        QString why;
        QVERIFY(!w.start(&why));
        QVERIFY(!w.isWatching());
        QVERIFY(!why.isEmpty());
        QDBusConnection::disconnectFromBus(QStringLiteral("clipitto-no-bus"));
    }

    void theFakeWatcherOnlyEmitsWhileWatching()
    {
        clipitto::FakeScreenLockWatcher fake;
        QSignalSpy spy(&fake, &clipitto::ScreenLockWatcher::locked);
        fake.simulateLock();
        QCOMPARE(spy.size(), 0);
        QVERIFY(fake.start());
        fake.simulateLock(QStringLiteral("sleep"));
        QCOMPARE(spy.size(), 1);
        fake.stop();
        fake.simulateLock();
        QCOMPARE(spy.size(), 1);
        clipitto::FakeScreenLockWatcher none(/*supported=*/false);
        QString why;
        QVERIFY(!none.start(&why));
        QVERIFY(!why.isEmpty());
    }
};

QTEST_MAIN(TestScreenLock)
#include "tst_screenlock.moc"
