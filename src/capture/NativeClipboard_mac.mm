// The macOS pasteboard read straight through AppKit, bypassing Qt's conversions: Qt turns
// pasteboard types into MIME types, which hides the nspasteboard.org markers password
// managers set; this reports the types as they are (UTIs).
// Apple's headers first: Qt's keyword macros (signals, slots, emit) must not meet them.
#import <AppKit/AppKit.h>

#include "NativeClipboard.h"

#include "ClipFilter.h"

namespace clipitto::capture {

namespace {

// A picture as PNG bytes: the pasteboard's own PNG, else its TIFF converted (what Preview
// and screenshots offer), else nothing.
QByteArray pictureAsPng(NSPasteboard* pasteboard)
{
    NSData* png = [pasteboard dataForType:NSPasteboardTypePNG];
    if (png.length > 0) {
        return QByteArray(static_cast<const char*>(png.bytes), qsizetype(png.length));
    }
    NSData* tiff = [pasteboard dataForType:NSPasteboardTypeTIFF];
    if (tiff.length == 0) {
        return {};
    }
    NSBitmapImageRep* bitmap = [NSBitmapImageRep imageRepWithData:tiff];
    NSData* converted = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return converted.length > 0 ? QByteArray(static_cast<const char*>(converted.bytes), qsizetype(converted.length))
                                : QByteArray();
}

} // namespace

qint64 clipboardChangeCount()
{
    return qint64([NSPasteboard generalPasteboard].changeCount);
}

NativeClipboardRead readNativeClipboard(bool readMarkedText)
{
    NativeClipboardRead result;
    @autoreleasepool {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        for (NSPasteboardType type in pasteboard.types) {
            result.formats << QString::fromNSString(type);
        }
        result.status = NativeClipboardRead::Status::Ok;

        const bool marked = ClipFilter::isSensitive(result.formats);
        const bool transient = ClipFilter::isTransient(result.formats);
        if ((marked && !readMarkedText) || (transient && !marked)) {
            // Only the markers: the pipeline drops it (or seals it, once passwords are kept),
            // and what a secret or temporary copy holds is never read.
            return result;
        }

        NSString* text = [pasteboard stringForType:NSPasteboardTypeString];
        if (text.length > 0) {
            result.text = QString::fromNSString(text);
            result.text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
            return result;
        }
        if (marked) {
            return result; // marked pictures are never read
        }
        const QByteArray png = pictureAsPng(pasteboard);
        if (!png.isEmpty()) {
            result.image = png;
            result.imageType = QStringLiteral("image/png");
            return result;
        }
        result.status = NativeClipboardRead::Status::NoText; // files, rich objects: not recorded
    }
    return result;
}

} // namespace clipitto::capture
