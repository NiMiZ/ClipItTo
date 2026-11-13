#pragma once

#include "ControlChannel.h"

#include <QDBusConnection>
#include <QDBusContext>

namespace clipitto::ipc {

// ControlChannel over the D-Bus session bus. The generated ControlAdaptor (from
// io.github.nimiz.ClipItTo.xml) forwards incoming calls to the public slots below.
class DbusControlChannel : public ControlChannel, protected QDBusContext {
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
    // A text copy with its HTML version (from the GNOME extension); never for a marked copy.
    void IngestRich(const QString& text, const QString& html, const QStringList& mimeTypes);
    // Only from GNOME Shell (where the ClipItTo extension runs): pictures are big, and no
    // other session client should be able to slip one into the history.
    void IngestImage(const QByteArray& data, const QString& mimeType, const QStringList& mimeTypes);
    void ReloadConfig();
    void LockPasswords();
    void SyncNow();
    void Quit();
    QVariantMap Status();
    bool WantsPasswords();

signals:
    // Relayed to D-Bus by the adaptor.
    void VisibilityChanged(bool visible);

private:
    // True for in-process calls and calls from GNOME Shell (the extension); logs a refusal.
    bool calledFromShell(const char* method);

    QDBusConnection m_bus;
    bool m_registered = false;
};

} // namespace clipitto::ipc
