#pragma once

#include "CaptureBackend.h"

namespace clipitto::capture {

// Capture through the ClipItTo GNOME Shell extension. The extension pushes every clipboard
// change to the daemon's Ingest D-Bus method (ControlChannel::ingestRequested), so this
// backend has nothing to run; it exists so selection and status can name the active source.
class GnomeExtBackend : public CaptureBackend {
    Q_OBJECT

public:
    using CaptureBackend::CaptureBackend;

    QString name() const override { return QStringLiteral("gnome-ext"); }
    void start() override {}
    void stop() override {}
};

} // namespace clipitto::capture
