#include "GnomeShellIntegration.h"

#include "DbusNames.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingReply>
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

void GnomeShellIntegration::call(const QString& method, const QVariantList& args,
                                  const std::function<void()>& ifUnknown)
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
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [method, ifUnknown](QDBusPendingCallWatcher* w) {
        if (w->isError() && ifUnknown && w->error().type() == QDBusError::UnknownMethod) {
            ifUnknown(); // an older extension, still loaded in the shell until the next login
        } else if (w->isError()) {
            qCWarning(lcShell).noquote() << "extension call" << method << "failed:" << w->error().message();
        }
        w->deleteLater();
    });
}

void GnomeShellIntegration::activatePopup()
{
    call(QStringLiteral("ActivateWindow"));
}

void GnomeShellIntegration::activatePopup(PopupPosition position)
{
    call(QStringLiteral("ActivateWindowAt"), {popupPositionName(position)},
         [this] { call(QStringLiteral("ActivateWindow")); });
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
    // An extension older than ctrl+shift+v types plain ctrl+v for the name it does not know.
    call(QStringLiteral("Paste"), {pastekeys::chordName(keys)});
}

void GnomeShellIntegration::rememberPasteTarget()
{
    const quint64 ask = ++m_targetAsk;
    m_target = PasteTarget::unknown(); // never a stale answer from the previous time
    if (!m_available) {
        m_target = PasteTarget::unsupported();
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kShellServiceName),
                                                      QString::fromLatin1(kShellObjectPath),
                                                      QString::fromLatin1(kShellInterface),
                                                      QStringLiteral("FocusedApp"));
    msg.setAutoStartService(false);
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, ask](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (ask != m_targetAsk) {
            return;
        }
        if (w->isError()) {
            // An older extension (still loaded in the shell until the next login) does not know the method.
            m_target = w->error().type() == QDBusError::UnknownMethod ? PasteTarget::unsupported() : PasteTarget::unknown();
            return;
        }
        const QDBusPendingReply<QStringList> reply = *w;
        QStringList ids;
        for (const QString& id : reply.value()) {
            if (!id.trimmed().isEmpty()) {
                ids << id.trimmed().left(pastekeys::kMaxPatternLength * 2);
            }
        }
        m_target = ids.isEmpty() ? PasteTarget::unknown() : PasteTarget::known(ids);
    });
}

} // namespace clipitto::ipc
