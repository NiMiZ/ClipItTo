#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include "MacPaste.h"

#include <unistd.h>

namespace clipitto {

namespace {

NSRunningApplication* g_target = nil; // where the paste goes

constexpr CGKeyCode kKeyV = 9; // kVK_ANSI_V: the key's position, whatever the layout

void pressCommandV()
{
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    for (const bool down : {true, false}) {
        CGEventRef event = CGEventCreateKeyboardEvent(source, kKeyV, down);
        CGEventSetFlags(event, kCGEventFlagMaskCommand);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
    if (source) {
        CFRelease(source);
    }
}

} // namespace

void macRememberPasteTarget()
{
    NSRunningApplication* front = [NSWorkspace sharedWorkspace].frontmostApplication;
    if (front && front.processIdentifier != getpid()) {
        g_target = front; // our own popup is no target: keep the previous one
    }
}

bool macAccessibilityGranted()
{
    return AXIsProcessTrusted();
}

bool macPasteCommandV()
{
    if (!AXIsProcessTrusted()) {
        // Ask: macOS shows its permission dialog (and lists ClipItTo in the Accessibility pane).
        NSDictionary* options = @{(__bridge id)kAXTrustedCheckOptionPrompt : @YES};
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
        return false;
    }
    NSRunningApplication* target = g_target;
    const bool wait = target && !target.terminated && !target.active;
    if (target && !target.terminated) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [target activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
    }
    if (wait) {
        // Activation is asynchronous: keys sent now could still reach the app that was in front.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            pressCommandV();
        });
    } else {
        pressCommandV();
    }
    return true;
}

} // namespace clipitto
