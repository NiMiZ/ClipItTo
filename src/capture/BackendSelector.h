#pragma once

#include "CaptureBackend.h"

#include <memory>

class QClipboard;

namespace clipitto {
struct Config;
struct SessionEnv;
} // namespace clipitto

namespace clipitto::capture {

// What the running session offers beyond QClipboard.
struct Capabilities {
    bool gnomeExtension = false; // the ClipItTo GNOME Shell extension is running
    bool wlDataControl = false; // `wl-paste --watch` works (wlr/ext data-control protocol)
};

struct Selection {
    std::unique_ptr<CaptureBackend> backend;
    bool degraded = false; // the backend cannot see every clipboard change in this session
    QString note; // human-readable reason, for logs and status
};

// Picks the best backend for the session, honoring Config::backend when it is usable:
//   Wayland: GNOME extension > wl-paste (data-control) > QClipboard (focused changes only)
//   X11, Windows: QClipboard (complete there)
Selection selectBackend(const Config& config, const SessionEnv& session, const Capabilities& caps,
                        QClipboard* clipboard, const QString& wlPasteProgram = QStringLiteral("wl-paste"));

} // namespace clipitto::capture
