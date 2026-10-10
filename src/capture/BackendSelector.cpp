#include "BackendSelector.h"

#include "Config.h"
#include "GnomeExtBackend.h"
#include "QtClipboardBackend.h"
#include "SessionEnv.h"
#include "WlPasteBackend.h"

namespace clipitto::capture {

namespace {

enum class Kind { GnomeExt, WlPaste, QtClipboard };

} // namespace

Selection selectBackend(const Config& config, const SessionEnv& session, const Capabilities& caps,
                        QClipboard* clipboard, const QString& wlPasteProgram)
{
    const bool wayland = session.display == SessionEnv::Display::Wayland;
    Kind kind = Kind::QtClipboard;
    QString note;

    switch (config.backend) {
    case BackendChoice::GnomeExt:
        if (caps.gnomeExtension) {
            kind = Kind::GnomeExt;
            note = QStringLiteral("configured: gnome-ext");
        } else {
            note = QStringLiteral("configured gnome-ext, but the ClipItTo GNOME Shell extension is not running");
        }
        break;
    case BackendChoice::WlPaste:
        kind = Kind::WlPaste; // explicit choice: trust it even without a successful probe
        note = QStringLiteral("configured: wl-paste");
        break;
    case BackendChoice::QtClipboard:
        note = QStringLiteral("configured: qt-clipboard");
        break;
    case BackendChoice::Auto:
        if (wayland && caps.gnomeExtension) {
            kind = Kind::GnomeExt;
            note = QStringLiteral("auto: GNOME Shell extension");
        } else if (wayland && caps.wlDataControl) {
            kind = Kind::WlPaste;
            note = QStringLiteral("auto: compositor supports data-control");
        } else {
            note = QStringLiteral("auto");
        }
        break;
    }

    Selection s;
    switch (kind) {
    case Kind::GnomeExt:
        s.backend = std::make_unique<GnomeExtBackend>();
        break;
    case Kind::WlPaste:
        s.backend = std::make_unique<WlPasteBackend>(wlPasteProgram);
        break;
    case Kind::QtClipboard: {
        s.backend = std::make_unique<QtClipboardBackend>(clipboard, config.capturePrimary);
        // QClipboard sees every change on X11 and Windows, but only focused changes on Wayland.
        const bool complete = session.display == SessionEnv::Display::X11
            || session.display == SessionEnv::Display::Windows;
        if (!complete) {
            s.degraded = true;
            note += wayland ? QStringLiteral("; enable the ClipItTo GNOME Shell extension for full capture, "
                                             "until then clipboard changes are only seen while ClipItTo has focus")
                            : QStringLiteral("; unknown session type, clipboard changes may be missed");
        }
        break;
    }
    }
    s.note = note;
    return s;
}

} // namespace clipitto::capture
