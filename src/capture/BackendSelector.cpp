#include "BackendSelector.h"

#include "Config.h"
#include "QtClipboardBackend.h"
#include "SessionEnv.h"

namespace clipitto::capture {

Selection selectBackend(const Config& config, const SessionEnv& session, QClipboard* clipboard)
{
    Selection s;
    s.backend = std::make_unique<QtClipboardBackend>(clipboard, config.capturePrimary);

    switch (config.backend) {
    case BackendChoice::GnomeExt:
    case BackendChoice::WlPaste:
        s.note = QStringLiteral("configured backend is not available in this build; using qt-clipboard");
        break;
    case BackendChoice::QtClipboard:
        s.note = QStringLiteral("configured: qt-clipboard");
        break;
    case BackendChoice::Auto:
        s.note = QStringLiteral("auto");
        break;
    }

    // QClipboard sees every change on X11 and Windows, but only focused changes on Wayland.
    const bool complete = session.display == SessionEnv::Display::X11
        || session.display == SessionEnv::Display::Windows;
    if (!complete) {
        s.degraded = true;
        s.note += QStringLiteral("; no complete capture backend for this session yet, clipboard "
                                 "changes are only seen while ClipItTo has focus");
    }
    return s;
}

} // namespace clipitto::capture
