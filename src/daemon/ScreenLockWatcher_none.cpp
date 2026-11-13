#include "ScreenLockWatcher.h"

#include <QCoreApplication>

namespace clipitto {

namespace {

// No screen-lock events in this build (no D-Bus, not Windows or macOS).
class NoScreenLockWatcher : public ScreenLockWatcher {
public:
    bool start(QString* why) override
    {
        if (why) {
            *why = QCoreApplication::translate("clipitto::Daemon", "this build has no screen-lock events");
        }
        return false;
    }
    void stop() override {}
    bool isWatching() const override { return false; }
};

} // namespace

std::unique_ptr<ScreenLockWatcher> createScreenLockWatcher()
{
    return std::make_unique<NoScreenLockWatcher>();
}

} // namespace clipitto
