#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>

namespace clipitto {

// The longest side of a stored thumbnail, in pixels.
inline constexpr int kThumbnailSize = 320;

// A copied picture, ready to store: encoded once, off the GUI thread (capture/ImageCodec).
struct ImageClip {
    QByteArray png; // the picture, lossless
    QByteArray thumbnail; // PNG, at most kThumbnailSize on each side
    int width = 0;
    int height = 0;
    QString pixelHash; // sha256 (hex) of the pixels: the same picture is one history entry
    // When it was copied (UTC): stored as its time, so a picture that took a while to encode
    // still sorts before text copied after it. Invalid: when it is stored.
    QDateTime copiedAt;

    bool isValid() const { return !png.isEmpty() && width > 0 && height > 0 && !pixelHash.isEmpty(); }
    // What the history shows and search matches, e.g. "Image 1920×1080".
    QString label() const { return QStringLiteral("Image %1×%2").arg(width).arg(height); }
};

} // namespace clipitto

Q_DECLARE_METATYPE(clipitto::ImageClip)
