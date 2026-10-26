#import <AppKit/AppKit.h>

#include "MacApp.h"

namespace clipitto::ui {

void makeBackgroundApp()
{
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

void bringAppToFront()
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // macOS 14 deprecates the argument, not the need
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
}

} // namespace clipitto::ui
