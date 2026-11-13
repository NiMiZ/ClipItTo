#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace clipitto {

// Tells the daemon when the user's screen locks or the PC goes to sleep, so the password vault can lock itself.
// One implementation per platform, chosen in CMake:
//   Windows: WTS session notifications + WM_POWERBROADCAST on a hidden window - ScreenLockWatcher_win.cpp
//   macOS:   screen-lock and sleep notifications (Foundation / AppKit)         - ScreenLockWatcher_mac.mm
//   Linux:   systemd-logind and screensaver signals over D-Bus (QtDBus)        - ScreenLockWatcher_dbus.cpp
//   else:    explicitly unsupported                                            - ScreenLockWatcher_none.cpp
// The watcher only reports; what a lock event does (and whether the setting allows it) is the daemon's decision.
class ScreenLockWatcher : public QObject {
    Q_OBJECT

public:
    explicit ScreenLockWatcher(QObject* parent = nullptr) : QObject(parent) {}

    // Starts watching (again). false = this system gives no such events (`why` says it); nothing is watched then.
    virtual bool start(QString* why = nullptr) = 0;
    virtual void stop() = 0;
    virtual bool isWatching() const = 0;

signals:
    // The screen locked or the PC is going to sleep; `reason` is a short fixed word for the debug log
    // ("screen lock", "sleep", "disconnect"). Always delivered on the GUI thread, through the event loop.
    void locked(const QString& reason);
};

// This platform's watcher. Never null; start() says whether it can work here.
std::unique_ptr<ScreenLockWatcher> createScreenLockWatcher();

// Tests: a watcher that emits when told to.
class FakeScreenLockWatcher : public ScreenLockWatcher {
    Q_OBJECT

public:
    explicit FakeScreenLockWatcher(bool supported = true, QObject* parent = nullptr)
        : ScreenLockWatcher(parent), m_supported(supported)
    {
    }
    bool start(QString* why = nullptr) override
    {
        ++startCalls;
        if (!m_supported) {
            if (why) {
                *why = QStringLiteral("fake: unsupported");
            }
            return false;
        }
        m_watching = true;
        return true;
    }
    void stop() override
    {
        ++stopCalls;
        m_watching = false;
    }
    bool isWatching() const override { return m_watching; }
    // The system reports a lock; like the real ones, nothing is emitted while stopped.
    void simulateLock(const QString& reason = QStringLiteral("screen lock"))
    {
        if (m_watching) {
            emit locked(reason);
        }
    }

    int startCalls = 0;
    int stopCalls = 0;

private:
    bool m_supported;
    bool m_watching = false;
};

} // namespace clipitto
