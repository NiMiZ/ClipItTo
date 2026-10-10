#pragma once

#include "ControlChannel.h"

#include <QDBusConnection>

namespace clipitto::ipc {

// ControlChannel over the D-Bus session bus. The generated ControlAdaptor (from
// io.github.nimiz.ClipItTo.xml) forwards incoming calls to the public slots below.
class DbusControlChannel : public ControlChannel {
    Q_OBJECT

public:
    explicit DbusControlChannel(QDBusConnection bus, QObject* parent = nullptr);
    ~DbusControlChannel() override; // releases the bus name if we own it

    // Exports the object and claims the well-known name (no queueing, no replacement).
    StartResult start(QString* error = nullptr) override;

public slots: // D-Bus methods
    void Toggle(const QString& activationToken);
    void Show(const QString& activationToken);
    void Hide();
    void Ingest(const QString& text, const QStringList& mimeTypes);
    void ReloadConfig();
    void Quit();
    QVariantMap Status();

signals:
    // Relayed to D-Bus by the adaptor.
    void VisibilityChanged(bool visible);

private:
    QDBusConnection m_bus;
    bool m_registered = false;
};

} // namespace clipitto::ipc
