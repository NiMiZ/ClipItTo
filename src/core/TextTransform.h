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
    QString id; // stable, for the Paste as… setting: "upper", "json_pretty", ...
    QString label; // with an & mnemonic, for menus
};

// All transforms in menu order.
QList<Info> all();

// The transformed text, or nullopt when `text` can't be (not JSON, not Base64, not valid UTF-8 after
// decoding); `why` says so in words.
std::optional<QString> apply(Kind kind, const QString& text, QString* why = nullptr);

// --- Keyboard layouts: text typed with the wrong layout, converted key for key ---------------------------
//
// "xccv" typed on an English keyboard while the Hebrew layout was meant is "סבבה" on the same keys. A
// layout is identified by its language code: "en" (always there) and the optional ones from layouts().

struct Layout {
    QString id; // "he"
    QString name; // "Hebrew"
};

// The layouts that can be switched on besides English, for Settings: Hebrew, Russian, Greek.
QList<Layout> layouts();
// "English", "Hebrew", ... for any id (the id itself when unknown).
QString layoutName(const QString& id);

// The layouts to offer, from the setting (`keyboard_layouts`): "" is automatic (the one for the system
// language `localeTag`, e.g. "he-IL", if there is one), "none" is none, else ids like "he, ru".
QStringList enabledLayouts(const QString& setting, const QString& localeTag);

// The layout `text` seems typed in: of English and `enabled`, the one with most of its letters ("en" when
// there are no letters).
QString layoutOf(const QString& text, const QStringList& enabled);
// The layouts `text` can be converted to: English and `enabled`, without its own.
QStringList convertibleTo(const QString& text, const QStringList& enabled);

// `text` retyped on the same keys in layout `to`, as if it had been typed in layout `from`. nullopt (and
// `why`) when no letter or symbol of it has a key to convert.
std::optional<QString> convertLayout(const QString& text, const QString& from, const QString& to, QString* why = nullptr);

} // namespace texttransform
} // namespace clipitto
