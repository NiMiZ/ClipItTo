#pragma once

#include <QImage>
#include <QString>

namespace clipitto::ui {

// The text as a QR code: black modules on white with the four-module quiet zone scanners need, every module a
// whole number of pixels, at least `minSide` pixels on each side. A null image when the text is empty or does
// not fit in a QR code (about 2900 characters); `error` then says why (translated, for the user).
QImage qrImage(const QString& text, int minSide, QString* error = nullptr);

// The most bytes of UTF-8 a QR code can hold (version 40, lowest error correction).
constexpr int kQrMaxBytes = 2953;

} // namespace clipitto::ui
