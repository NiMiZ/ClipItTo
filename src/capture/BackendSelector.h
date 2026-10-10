#pragma once

#include "CaptureBackend.h"

#include <memory>

class QClipboard;

namespace clipitto {
struct Config;
struct SessionEnv;
} // namespace clipitto

namespace clipitto::capture {

struct Selection {
    std::unique_ptr<CaptureBackend> backend;
    bool degraded = false; // the backend cannot see every clipboard change in this session
    QString note; // human-readable reason, for logs and status
};

// Picks the best available backend for the session, honoring Config::backend when it is usable.
Selection selectBackend(const Config& config, const SessionEnv& session, QClipboard* clipboard);

} // namespace clipitto::capture
