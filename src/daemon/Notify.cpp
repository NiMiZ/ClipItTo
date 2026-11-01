#include "Notify.h"

#include <QPointer>
#include <QSystemTrayIcon>

#if defined(CLIPITTO_NOTIFY_DBUS)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QVariantMap>
#endif

namespace clipitto {

namespace {

#if defined(CLIPITTO_NOTIFY_DBUS)
bool notifyOverDbus(const QString& title, const QString& body)
{
    const QString service = QStringLiteral("org.freedesktop.Notifications");
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface() || !bus.interface()->isServiceRegistered(service)) {
        return false;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(service, QStringLiteral("/org/freedesktop/Notifications"),
                                                       service, QStringLiteral("Notify"));
    call << QStringLiteral("ClipItTo") << uint(0) << QStringLiteral("edit-paste") << title << body << QStringList()
         << QVariantMap() << int(5000);
    return !bus.asyncCall(call).isError(); // the reply is not awaited
}
#endif

QPointer<QSystemTrayIcon> g_tray;

bool notifyOverTray(const QString& title, const QString& body)
{
    if (!g_tray || !g_tray->isVisible() || !QSystemTrayIcon::supportsMessages()) {
        return false;
    }
    g_tray->showMessage(title, body, QSystemTrayIcon::Information, 5000);
    return true;
}

} // namespace

void setNotifyTray(QSystemTrayIcon* tray)
{
    g_tray = tray;
}

bool desktopNotify(const QString& title, const QString& body)
{
#if defined(CLIPITTO_NOTIFY_DBUS)
    if (notifyOverDbus(title, body)) {
        return true;
    }
#endif
    return notifyOverTray(title, body);
}

} // namespace clipitto
