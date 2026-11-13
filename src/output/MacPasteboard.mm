#import <AppKit/AppKit.h>

#include "MacPasteboard.h"

namespace clipitto {

long setConcealedText(const QString& text)
{
    @autoreleasepool {
        NSString* const concealed = @"org.nspasteboard.ConcealedType";
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard declareTypes:@[ NSPasteboardTypeString, concealed ] owner:nil];
        [pasteboard setString:text.toNSString() forType:NSPasteboardTypeString];
        // The marker's content doesn't matter, but the type must exist: try empty, then a byte.
        if (![pasteboard setString:@"" forType:concealed]) {
            [pasteboard setData:[NSData dataWithBytes:" " length:1] forType:concealed];
        }
        // Every write above changed the count; the last reading is what a later clear compares with.
        return long([pasteboard changeCount]);
    }
}

bool clearPasteboardIfUnchanged(long changeCount)
{
    @autoreleasepool {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        if (long([pasteboard changeCount]) != changeCount) {
            return false;
        }
        [pasteboard clearContents];
        return true;
    }
}

void clearPasteboard()
{
    @autoreleasepool {
        [[NSPasteboard generalPasteboard] clearContents];
    }
}

} // namespace clipitto
