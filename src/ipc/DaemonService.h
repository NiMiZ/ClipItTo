#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace clipitto::ipc {

// The daemon's D-Bus object. The generated DaemonServiceAdaptor (from
// io.github.nimiz.ClipItTo.xml) forwards incoming calls to the public slots below,
// which only re-emit them as signals: the daemon decides what they mean.
class DaemonService : public QObject {
    Q_OBJECT

public:
    enum class RegisterResult { Ok, AlreadyRunning, Failed };

    explicit DaemonService(QObject* parent = nullptr);
    ~DaemonService() override; // releases the bus name if we own it

    void setStatusProvider(std::function<QVariantMap()> provider) { m_statusProvider = std::move(provider); }

    // Exports the object and claims the well-known name (no queueing, no replacement).
    RegisterResult registerOn(QDBusConnection bus, QString* error = nullptr);

public slots:
    void Toggle(const QString& activationToken);
    void Show(const QString& activationToken);
    void Hide();
    void Ingest(const QString& text, const QStringList& mimeTypes);
    void ReloadConfig();
    QVariantMap Status();

signals:
    void toggleRequested(const QString& activationToken);
    void showRequested(const QString& activationToken);
    void hideRequested();
    void ingestRequested(const QString& text, const QStringList& mimeTypes);
    void reloadRequested();

    // Relayed to D-Bus by the adaptor.
    void VisibilityChanged(bool visible);

private:
    std::function<QVariantMap()> m_statusProvider;
    QDBusConnection m_bus{QString()};
    bool m_registered = false;
};

} // namespace clipitto::ipc
