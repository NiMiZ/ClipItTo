#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

// Reading the text in a picture, on this computer only: no network, no cloud service. One
// implementation per platform, chosen in CMake:
//   Windows:  Windows.Media.Ocr (built into Windows; uses the languages installed for the user)
//   macOS:    the Vision framework (built in)
//   Linux:    Tesseract, when it was found at build time (Ocr_tesseract.cpp); else unsupported
// Every call blocks and must not run on the GUI thread.
namespace clipitto::ocr {

// Whether text can be read here; the reason (and what to install) when not.
bool available(QString* why = nullptr);

// The text in `png` (a PNG picture), lines joined by "\n"; empty when there is none. nullopt
// with `error` set when it couldn't be read at all.
std::optional<QString> recognize(const QByteArray& png, QString* error = nullptr);

} // namespace clipitto::ocr
