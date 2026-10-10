#include "BackendSelector.h"

#include "Config.h"
#include "SessionEnv.h"
#include "X11QtBackend.h"

namespace clipitto::capture {

Selection selectBackend(const Config& config, const SessionEnv& session, QClipboard* clipboard)
{
    Selection s;
    s.backend = std::make_unique<X11QtBackend>(clipboard, config.capturePrimary);

    switch (config.backend) {
    case BackendChoice::GnomeExt:
    case BackendChoice::WlPaste:
        s.degraded = session.display != SessionEnv::Display::X11;
        s.note = QStringLiteral("configured backend is not available in this build; using x11");
        return s;
    case BackendChoice::X11:
        s.note = QStringLiteral("configured: x11");
        break;
    case BackendChoice::Auto:
        s.note = QStringLiteral("auto");
        break;
    }

    if (session.display != SessionEnv::Display::X11) {
        s.degraded = true;
        s.note += QStringLiteral("; no Wayland capture backend available, clipboard changes are "
                                 "only seen while ClipItTo has focus");
    }
    return s;
}

} // namespace clipitto::capture
