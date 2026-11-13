#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace clipitto {

// Which items the "Paste as…" menu (Shift+Enter, and the right-click submenu) shows, and in what order. Every
// item has a stable id; the setting (`paste_as_text`, `paste_as_picture`) is a comma list of ids in menu
// order, a "-" before an id hides it: "upper,-lower,title". Empty means the built-in list.
namespace pasteas {

struct Item {
    QString id;
    QString label; // with an & mnemonic, for menus
};

// Ids of the items that are not text transforms.
inline constexpr const char* kKeyboardLayout = "layout";
inline constexpr const char* kTranslate = "translate";
inline constexpr const char* kRich = "rich"; // only offered for a copy that has a formatted version
inline constexpr const char* kNumber = "number"; // only offered for a copy that is a number: its set bits
inline constexpr const char* kPictureText = "pic_text";
inline constexpr const char* kPictureChanged = "pic_changed";
inline constexpr const char* kPictureTranslate = "pic_translate";
inline constexpr const char* kPicturePaste = "pic_paste";
inline constexpr const char* kPasteOnce = "once"; // a toggle: forget the copy right after it is pasted; for text and pictures

// The built-in items in the built-in order, for a text copy and for a picture.
QList<Item> textItems();
QList<Item> pictureItems();

struct Choice {
    QString id;
    bool shown = true;
    bool operator==(const Choice&) const = default;
};

// The setting applied to `items`: its order and visibility; unknown and repeated ids are ignored; items it
// does not mention (added in a later version) come last, shown.
QList<Choice> resolve(const QString& setting, const QList<Item>& items);
// The setting for `choices`; empty when they are the built-in list.
QString serialize(const QList<Choice>& choices, const QList<Item>& items);
// The ids of the shown items, in order.
QStringList shownIds(const QString& setting, const QList<Item>& items);

// `label` without its & mnemonic marker.
QString plain(const QString& label);
// The labels with a repeated mnemonic letter fixed: the later one loses its & (case-insensitive).
QStringList uniqueMnemonics(const QStringList& labels);

} // namespace pasteas
} // namespace clipitto
