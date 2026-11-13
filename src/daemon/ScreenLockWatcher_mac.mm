#include "ScreenLockWatcher.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <QMetaObject>
#include <QPointer>

namespace clipitto {

namespace {

// com.apple.screenIsLocked (posted by the login window, to the distributed center), and the workspace's
// will-sleep / screens-did-sleep. The blocks run on the main queue, which is the GUI thread; they only queue the signal.
class MacScreenLockWatcher : public ScreenLockWatcher {
public:
    ~MacScreenLockWatcher() override { stop(); }

    bool start(QString*) override
    {
        if (!m_observers) {
            m_observers = [NSMutableArray array];
            add([NSDistributedNotificationCenter defaultCenter], @"com.apple.screenIsLocked", "screen lock");
            NSNotificationCenter* workspace = [[NSWorkspace sharedWorkspace] notificationCenter];
            add(workspace, NSWorkspaceWillSleepNotification, "sleep");
            add(workspace, NSWorkspaceScreensDidSleepNotification, "sleep");
        }
        return true;
    }

    void stop() override
    {
        for (NSArray* pair in m_observers) { // [center, token]
            [pair[0] removeObserver:pair[1]];
        }
        m_observers = nil;
    }

    bool isWatching() const override { return m_observers != nil; }

private:
    void add(NSNotificationCenter* center, NSString* name, const char* reason)
    {
        QPointer<MacScreenLockWatcher> self(this);
        const QString text = QString::fromLatin1(reason);
        id token = [center addObserverForName:name
                                       object:nil
                                        queue:[NSOperationQueue mainQueue]
                                   usingBlock:^(NSNotification*) {
                                       if (self) {
                                           QMetaObject::invokeMethod(
                                               self.data(), [self, text] { if (self) emit self->locked(text); },
                                               Qt::QueuedConnection);
                                       }
                                   }];
        [m_observers addObject:@[ center, token ]];
    }

    NSMutableArray* m_observers = nil;
};

} // namespace

std::unique_ptr<ScreenLockWatcher> createScreenLockWatcher()
{
    return std::make_unique<MacScreenLockWatcher>();
}

} // namespace clipitto
