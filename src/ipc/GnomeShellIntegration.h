#pragma once

#include "DesktopIntegration.h"

#include <QDBusConnection>

#include <functional>

class QDBusServiceWatcher;

namespace clipitto::ipc {

// Talks to the ClipItTo GNOME Shell extension over the session bus. Availability follows
// the extension's bus name, so enabling/disabling it or restarting the shell is noticed.
class GnomeShellIntegration : public DesktopIntegration {
    Q_OBJECT

public:
    explicit GnomeShellIntegration(QDBusConnection bus, QObject* parent = nullptr);

    QString name() const override { return QStringLiteral("gnome-shell-extension"); }
    bool isAvailable() const override { return m_available; }

    void activatePopup() override;
    void activatePopup(PopupPosition position) override;
    void setClipboard(const QString& text) override;
    void setClipboardImage(const QByteArray& png) override;
    void paste(PasteKeys keys) override;

private:
    void call(const QString& method, const QVariantList& args = {}, const std::function<void()>& ifUnknown = {});
    void setAvailable(bool available);

    QDBusConnection m_bus;
    QDBusServiceWatcher* m_watcher;
    bool m_available = false;
};

} // namespace clipitto::ipc
