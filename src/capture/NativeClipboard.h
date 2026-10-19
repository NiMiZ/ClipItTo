#pragma once

#include <QString>
#include <QStringList>

namespace clipitto::capture {

// The clipboard's text read straight from the OS, bypassing Qt's format conversion.
//
// On Windows, Qt's text reader falls back to the ANSI copy (CF_TEXT) whenever reading the
// Unicode copy fails for a moment, e.g. while a Chromium-based app such as WhatsApp Desktop
// is still serving it. Every character outside the system code page then becomes '?'
// (Hebrew on an English system). This reader takes CF_UNICODETEXT only, and reports Busy
// instead of falling back, so the caller can try again shortly.
struct NativeClipboardRead {
    enum class Status {
        Ok, // text, or a picture (image, imageType), and formats are filled in; both stay
            // empty when a privacy marker is set
        NoText, // the clipboard holds neither text nor a picture (formats may be filled in)
        Busy, // another program holds the clipboard, or the text isn't ready yet: retry
        Unsupported, // no native reader on this platform: use QClipboard
    };
    Status status = Status::Unsupported;
    QString text;
    // A picture when the copy has no text, read in the same clipboard session as the markers:
    // the "PNG" format (image/png) or the device-independent bitmap as a BMP file (image/bmp).
    QByteArray image;
    QString imageType;
    // Format names, with privacy markers that apply reported under the names
    // ClipFilter::isSensitive() knows (e.g. "ExcludeClipboardContentFromMonitorProcessing").
    QStringList formats;
};

NativeClipboardRead readNativeClipboard();

} // namespace clipitto::capture
