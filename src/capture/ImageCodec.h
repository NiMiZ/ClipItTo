#pragma once

#include "ImageClip.h"

#include <QByteArray>
#include <QImage>

#include <functional>
#include <optional>

class QObject;

namespace clipitto::capture {

// Copied pictures, ready for the history: PNG (lossless), a thumbnail and a hash of the
// pixels, so the same picture copied twice (or our own paste coming back) is recognized
// whatever format the source app offered it in.

// The picture formats accepted from outside (wl-paste, the GNOME extension). Others are
// refused rather than handed to whatever image plugin happens to be installed.
bool isSupportedImageType(const QString& mimeType);

// The largest picture handled, in pixels (checked before decoding when possible).
inline constexpr qint64 kMaxImagePixels = 80LL * 1000 * 1000;

// The sha256 (hex) of the picture's pixels, independent of its encoding.
QString pixelHash(const QImage& image);

// Synchronous: safe on any thread. nullopt for a null picture, one with more than
// kMaxImagePixels, or one whose PNG would exceed maxPngBytes.
std::optional<ImageClip> encodeImage(const QImage& image, qint64 maxPngBytes);

// encodeImage on a worker thread (a 4K screenshot takes a noticeable moment to encode).
// `done` runs on `context`'s thread, and not at all if `context` is destroyed first.
void encodeImageAsync(const QImage& image, qint64 maxPngBytes, QObject* context,
                      std::function<void(std::optional<ImageClip>)> done);
// The same for an encoded picture as wl-paste or the GNOME extension hand it over: decoded
// on the worker as `mimeType` (one of isSupportedImageType), its size checked first.
void encodeImageDataAsync(const QByteArray& data, const QString& mimeType, qint64 maxPngBytes, QObject* context,
                          std::function<void(std::optional<ImageClip>)> done);

} // namespace clipitto::capture
