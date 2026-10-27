#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace clipitto {

// Changes to a copy's text applied while pasting it ("Paste as…"). The stored copy is untouched.
namespace texttransform {

enum class Kind {
    Upper,
    Lower,
    Title, // First Letter Of Each Word Capitalised
    Trim, // leading and trailing white space
    CollapseSpaces, // runs of spaces and tabs become one space, lines keep their breaks
    OneLine, // line breaks become spaces
    JsonPretty,
    JsonMinify,
    UrlEncode,
    UrlDecode,
    Base64Encode,
    Base64Decode,
};

struct Info {
    Kind kind;
    QString label; // with an & mnemonic, for menus
};

// All transforms in menu order.
QList<Info> all();

// The transformed text, or nullopt when `text` can't be (not JSON, not Base64, not valid UTF-8 after
// decoding); `why` says so in words.
std::optional<QString> apply(Kind kind, const QString& text, QString* why = nullptr);

} // namespace texttransform
} // namespace clipitto
