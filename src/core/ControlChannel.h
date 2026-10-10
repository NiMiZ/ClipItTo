#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace clipitto {

// How the outside world drives the daemon: requests from shortcuts / the CLI / capture
// helpers come in as signals; status and popup visibility go out.
// Platform implementations: ipc::DbusControlChannel (Linux).
class ControlChannel : public QObject {
    Q_OBJECT

public:
    enum class StartResult { Ok, AlreadyRunning, Failed };

    using QObject::QObject;

    // Claims single-instance ownership and starts accepting requests.
    virtual StartResult start(QString* error = nullptr) = 0;

    void setStatusProvider(std::function<QVariantMap()> provider) { m_statusProvider = std::move(provider); }
    QVariantMap status() const { return m_statusProvider ? m_statusProvider() : QVariantMap{}; }

signals:
    void toggleRequested(const QString& activationToken);
    void showRequested(const QString& activationToken);
    void hideRequested();
    void ingestRequested(const QString& text, const QStringList& mimeTypes);
    void reloadRequested();

    // Emitted by the daemon when the popup is shown or hidden; implementations forward it to clients.
    void visibilityChanged(bool visible);

private:
    std::function<QVariantMap()> m_statusProvider;
};

} // namespace clipitto
