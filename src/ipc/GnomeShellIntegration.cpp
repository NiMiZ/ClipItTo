#include "GnomeShellIntegration.h"

#include "DbusNames.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcShell, "clipitto.shell")

namespace clipitto::ipc {

GnomeShellIntegration::GnomeShellIntegration(QDBusConnection bus, QObject* parent)
    : DesktopIntegration(parent)
    , m_bus(std::move(bus))
    , m_watcher(new QDBusServiceWatcher(QString::fromLatin1(kShellServiceName), m_bus,
                                        QDBusServiceWatcher::WatchForOwnerChange, this))
{
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { setAvailable(true); });
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { setAvailable(false); });
    if (m_bus.isConnected()) {
        m_available = m_bus.interface()->isServiceRegistered(QString::fromLatin1(kShellServiceName)).value();
    }
}

void GnomeShellIntegration::setAvailable(bool available)
{
    if (available != m_available) {
        m_available = available;
        qCInfo(lcShell) << "GNOME Shell extension" << (available ? "connected" : "gone");
        emit availabilityChanged(available);
    }
}

void GnomeShellIntegration::call(const QString& method, const QVariantList& args)
{
    if (!m_available) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kShellServiceName),
                                                      QString::fromLatin1(kShellObjectPath),
                                                      QString::fromLatin1(kShellInterface), method);
    msg.setArguments(args);
    msg.setAutoStartService(false);
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [method](QDBusPendingCallWatcher* w) {
        if (w->isError()) {
            qCWarning(lcShell).noquote() << "extension call" << method << "failed:" << w->error().message();
        }
        w->deleteLater();
    });
}

void GnomeShellIntegration::activatePopup()
{
    call(QStringLiteral("ActivateWindow"));
}

void GnomeShellIntegration::setClipboard(const QString& text)
{
    call(QStringLiteral("SetClipboard"), {text});
}

void GnomeShellIntegration::setClipboardImage(const QByteArray& png)
{
    call(QStringLiteral("SetClipboardContent"), {png, QStringLiteral("image/png")});
}

void GnomeShellIntegration::paste(PasteKeys keys)
{
    call(QStringLiteral("Paste"),
         {keys == PasteKeys::ShiftInsert ? QStringLiteral("shift+insert") : QStringLiteral("ctrl+v")});
}

} // namespace clipitto::ipc
