#pragma once

#include "Config.h"

#include <QObject>

namespace clipitto {

// Optional helper living inside the desktop shell (on Linux: the ClipItTo GNOME Shell
// extension) that can do what a Wayland client cannot: activate our popup, own the
// clipboard without having focus, and type the paste shortcut into another app.
// It may come and go at runtime (extension enabled/disabled, shell restarted).
class DesktopIntegration : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString name() const = 0;
    virtual bool isAvailable() const = 0;

    virtual void activatePopup() = 0; // focus the (just shown) popup window
    virtual void setClipboard(const QString& text) = 0;
    virtual void paste(PasteKeys keys) = 0;

signals:
    void availabilityChanged(bool available);
};

} // namespace clipitto
