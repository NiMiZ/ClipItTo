#import <AppKit/AppKit.h>

#include "SourceApp.h"

#include <unistd.h>

namespace clipitto::capture {

// The app in front when the copy happened (⌘C is made in the frontmost app), by its name
// as the Dock shows it.
QString clipboardSourceApp()
{
    @autoreleasepool {
        NSRunningApplication* app = [NSWorkspace sharedWorkspace].frontmostApplication;
        if (!app || app.processIdentifier == getpid()) {
            return {}; // our own popup is in front: not where the copy came from
        }
        return QString::fromNSString(app.localizedName);
    }
}

} // namespace clipitto::capture
