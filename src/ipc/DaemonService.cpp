#include "DaemonService.h"

#include "DaemonServiceAdaptor.h"
#include "DbusNames.h"

#include <QDBusConnectionInterface>

namespace clipitto::ipc {

DaemonService::DaemonService(QObject* parent) : QObject(parent)
{
    new DaemonServiceAdaptor(this); // owned by this
}

DaemonService::~DaemonService()
{
    if (m_registered) {
        m_bus.unregisterService(QString::fromLatin1(kServiceName));
        m_bus.unregisterObject(QString::fromLatin1(kObjectPath));
    }
}

DaemonService::RegisterResult DaemonService::registerOn(QDBusConnection bus, QString* error)
{
    const auto fail = [&](RegisterResult result, const QString& message) {
        if (error) {
            *error = message;
        }
        return result;
    };

    if (!bus.isConnected()) {
        return fail(RegisterResult::Failed, QStringLiteral("not connected to the session bus: %1")
                                                .arg(bus.lastError().message()));
    }
    if (!bus.registerObject(QString::fromLatin1(kObjectPath), this)) {
        return fail(RegisterResult::Failed, QStringLiteral("cannot export %1").arg(QLatin1String(kObjectPath)));
    }

    const auto reply = bus.interface()->registerService(QString::fromLatin1(kServiceName),
                                                        QDBusConnectionInterface::DontQueueService,
                                                        QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid()) {
        bus.unregisterObject(QString::fromLatin1(kObjectPath));
        return fail(RegisterResult::Failed, reply.error().message());
    }
    if (reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        bus.unregisterObject(QString::fromLatin1(kObjectPath));
        return fail(RegisterResult::AlreadyRunning,
                    QStringLiteral("%1 is already owned by another process").arg(QLatin1String(kServiceName)));
    }
    m_bus = bus;
    m_registered = true;
    return RegisterResult::Ok;
}

void DaemonService::Toggle(const QString& activationToken)
{
    emit toggleRequested(activationToken);
}

void DaemonService::Show(const QString& activationToken)
{
    emit showRequested(activationToken);
}

void DaemonService::Hide()
{
    emit hideRequested();
}

void DaemonService::Ingest(const QString& text, const QStringList& mimeTypes)
{
    emit ingestRequested(text, mimeTypes);
}

void DaemonService::ReloadConfig()
{
    emit reloadRequested();
}

QVariantMap DaemonService::Status()
{
    return m_statusProvider ? m_statusProvider() : QVariantMap{};
}

} // namespace clipitto::ipc
