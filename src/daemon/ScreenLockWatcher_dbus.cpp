#include "ScreenLockWatcher_dbus.h"

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcScreenLock, "clipitto.screenlock")

namespace clipitto {

namespace {

constexpr char kLogindService[] = "org.freedesktop.login1";
constexpr char kLogindPath[] = "/org/freedesktop/login1";
constexpr char kLogindManager[] = "org.freedesktop.login1.Manager";
constexpr char kLogindSession[] = "org.freedesktop.login1.Session";

} // namespace

DbusScreenLockWatcher::DbusScreenLockWatcher(const QDBusConnection& systemBus, const QDBusConnection& sessionBus,
                                             const QString& sessionPath, QObject* parent)
    : ScreenLockWatcher(parent), m_system(systemBus), m_sessionBus(sessionBus), m_sessionPath(sessionPath)
{
}

DbusScreenLockWatcher::~DbusScreenLockWatcher()
{
    stop();
}

bool DbusScreenLockWatcher::subscribe(const QDBusConnection& bus, const QString& service, const QString& path,
                                      const QString& interface, const QString& name, const char* slot)
{
    QDBusConnection copy = bus;
    if (!copy.connect(service, path, interface, name, this, slot)) {
        qCInfo(lcScreenLock).noquote() << "could not subscribe to" << interface << name << "on" << path;
        return false;
    }
    m_subscriptions.append({bus, service, path, interface, name, QString::fromLatin1(slot)});
    return true;
}

bool DbusScreenLockWatcher::start(QString* why)
{
    stop();
    ++m_epoch;
    int subscribed = 0;
    if (m_system.isConnected()) {
        subscribed += subscribe(m_system, QString::fromLatin1(kLogindService), QString::fromLatin1(kLogindPath),
                                QString::fromLatin1(kLogindManager), QStringLiteral("PrepareForSleep"),
                                SLOT(onPrepareForSleep(bool)));
        if (m_sessionPath.isEmpty()) {
            lookUpSession(/*byPid=*/true);
        } else {
            subscribeSession(m_sessionPath);
        }
    } else {
        qCInfo(lcScreenLock) << "no system bus: logind lock and sleep events are not watched";
    }
    if (m_sessionBus.isConnected()) {
        // GNOME, and the freedesktop name that KDE and others use (at /org/freedesktop/ScreenSaver and /ScreenSaver).
        subscribed += subscribe(m_sessionBus, QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("/org/gnome/ScreenSaver"),
                                QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("ActiveChanged"),
                                SLOT(onScreenSaverActive(bool)));
        for (const char* path : {"/org/freedesktop/ScreenSaver", "/ScreenSaver"}) {
            subscribed += subscribe(m_sessionBus, QStringLiteral("org.freedesktop.ScreenSaver"), QString::fromLatin1(path),
                                    QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("ActiveChanged"),
                                    SLOT(onScreenSaverActive(bool)));
        }
    } else {
        qCInfo(lcScreenLock) << "no session bus: screensaver events are not watched";
    }
    m_watching = subscribed > 0;
    if (!m_watching && why) {
        *why = QCoreApplication::translate("clipitto::Daemon", "no D-Bus connection to systemd-logind or a screensaver");
    }
    return m_watching;
}

void DbusScreenLockWatcher::stop()
{
    for (Subscription& s : m_subscriptions) {
        s.bus.disconnect(s.service, s.path, s.interface, s.name, this, s.slot.toLatin1().constData());
    }
    m_subscriptions.clear();
    ++m_epoch;
    m_watching = false;
}

void DbusScreenLockWatcher::subscribeSession(const QString& path)
{
    subscribe(m_system, QString::fromLatin1(kLogindService), path, QString::fromLatin1(kLogindSession),
              QStringLiteral("Lock"), SLOT(onSessionLock()));
}

// The user's own session, so another user's Lock signal is not ours: GetSessionByPID first, then XDG_SESSION_ID.
void DbusScreenLockWatcher::lookUpSession(bool byPid)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(kLogindService), QString::fromLatin1(kLogindPath),
                                                       QString::fromLatin1(kLogindManager),
                                                       byPid ? QStringLiteral("GetSessionByPID") : QStringLiteral("GetSession"));
    if (byPid) {
        call << static_cast<quint32>(QCoreApplication::applicationPid());
    } else {
        const QString id = qEnvironmentVariable("XDG_SESSION_ID");
        if (id.isEmpty()) {
            qCInfo(lcScreenLock) << "no logind session found: the session Lock signal is not watched (sleep still is)";
            return;
        }
        call << id;
    }
    auto* watcher = new QDBusPendingCallWatcher(m_system.asyncCall(call, 5000), this);
    watcher->setProperty("byPid", byPid);
    watcher->setProperty("epoch", m_epoch);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &DbusScreenLockWatcher::onSessionPathFound);
}

void DbusScreenLockWatcher::onSessionPathFound(QDBusPendingCallWatcher* call)
{
    call->deleteLater();
    if (call->property("epoch").toULongLong() != m_epoch) {
        return; // stopped (or restarted) meanwhile
    }
    const QDBusPendingReply<QDBusObjectPath> reply = *call;
    if (reply.isError()) {
        if (call->property("byPid").toBool()) {
            lookUpSession(/*byPid=*/false);
        } else {
            qCInfo(lcScreenLock) << "no logind session found: the session Lock signal is not watched (sleep still is)";
        }
        return;
    }
    m_sessionPath = reply.value().path();
    subscribeSession(m_sessionPath);
}

void DbusScreenLockWatcher::onSessionLock()
{
    emit locked(QStringLiteral("screen lock"));
}

void DbusScreenLockWatcher::onPrepareForSleep(bool starting)
{
    if (starting) { // false = waking up again
        emit locked(QStringLiteral("sleep"));
    }
}

void DbusScreenLockWatcher::onScreenSaverActive(bool active)
{
    if (active) { // false = the screensaver ended; unlocking is ignored on purpose
        emit locked(QStringLiteral("screen lock"));
    }
}

std::unique_ptr<ScreenLockWatcher> createScreenLockWatcher()
{
    return std::make_unique<DbusScreenLockWatcher>(QDBusConnection::systemBus(), QDBusConnection::sessionBus());
}

} // namespace clipitto
