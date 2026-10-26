#import <AppKit/AppKit.h>

#include "MacPasteboard.h"

namespace clipitto {

void setConcealedText(const QString& text)
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
    }
}

} // namespace clipitto
