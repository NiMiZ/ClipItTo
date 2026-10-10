#include "DbusControlChannel.h"

#include "ControlAdaptor.h"
#include "DbusNames.h"

#include <QDBusConnectionInterface>

namespace clipitto::ipc {

DbusControlChannel::DbusControlChannel(QDBusConnection bus, QObject* parent)
    : ControlChannel(parent)
    , m_bus(std::move(bus))
{
    new ControlAdaptor(this); // owned by this
    connect(this, &ControlChannel::visibilityChanged, this, &DbusControlChannel::VisibilityChanged);
}

DbusControlChannel::~DbusControlChannel()
{
    if (m_registered) {
        m_bus.unregisterService(QString::fromLatin1(kServiceName));
        m_bus.unregisterObject(QString::fromLatin1(kObjectPath));
    }
}

ControlChannel::StartResult DbusControlChannel::start(QString* error)
{
    const auto fail = [&](StartResult result, const QString& message) {
        if (error) {
            *error = message;
        }
        return result;
    };

    if (!m_bus.isConnected()) {
        return fail(StartResult::Failed, QStringLiteral("not connected to the session bus: %1")
                                             .arg(m_bus.lastError().message()));
    }
    if (!m_bus.registerObject(QString::fromLatin1(kObjectPath), this)) {
        return fail(StartResult::Failed, QStringLiteral("cannot export %1").arg(QLatin1String(kObjectPath)));
    }

    const auto reply = m_bus.interface()->registerService(QString::fromLatin1(kServiceName),
                                                          QDBusConnectionInterface::DontQueueService,
                                                          QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid()) {
        m_bus.unregisterObject(QString::fromLatin1(kObjectPath));
        return fail(StartResult::Failed, reply.error().message());
    }
    if (reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        m_bus.unregisterObject(QString::fromLatin1(kObjectPath));
        return fail(StartResult::AlreadyRunning,
                    QStringLiteral("%1 is already owned by another process").arg(QLatin1String(kServiceName)));
    }
    m_registered = true;
    return StartResult::Ok;
}

void DbusControlChannel::Toggle(const QString& activationToken)
{
    emit toggleRequested(activationToken);
}

void DbusControlChannel::Show(const QString& activationToken)
{
    emit showRequested(activationToken);
}

void DbusControlChannel::Hide()
{
    emit hideRequested();
}

void DbusControlChannel::Ingest(const QString& text, const QStringList& mimeTypes)
{
    emit ingestRequested(text, mimeTypes);
}

void DbusControlChannel::ReloadConfig()
{
    emit reloadRequested();
}

QVariantMap DbusControlChannel::Status()
{
    return status();
}

} // namespace clipitto::ipc
