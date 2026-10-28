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

// Whether this build has text recognition at all (Windows, macOS, Linux with Tesseract). Cheap and always
// the same: no engine is started. Whether it can read a given language is found out when it is used.
bool builtIn();

// Whether text can be read here right now; the reason (and what to install) when not. Starts the engine.
bool available(QString* why = nullptr);

// The text in `png` (a PNG picture), lines joined by "\n"; empty when there is none. nullopt
// with `error` set when it couldn't be read at all.
std::optional<QString> recognize(const QByteArray& png, QString* error = nullptr);

} // namespace clipitto::ocr
