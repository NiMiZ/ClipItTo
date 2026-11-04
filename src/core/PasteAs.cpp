#include "PasteAs.h"

#include "TextTransform.h"

#include <QCoreApplication>
#include <QSet>

namespace clipitto::pasteas {

namespace {

QString t(const char* text)
{
    return QCoreApplication::translate("clipitto::pasteas", text);
}

} // namespace

QList<Item> textItems()
{
    QList<Item> items;
    for (const texttransform::Info& info : texttransform::all()) {
        items.append({info.id, info.label});
    }
    items.append({QString::fromLatin1(kKeyboardLayout), t("As other &keyboard layout")});
    items.append({QString::fromLatin1(kTranslate), t("Tran&slate")});
    items.append({QString::fromLatin1(kRich), t("With &formatting")});
    return items;
}

QList<Item> pictureItems()
{
    return {
        {QString::fromLatin1(kPictureText), t("Paste &text from picture")},
        {QString::fromLatin1(kPictureChanged), t("Text from picture, &changed")},
        {QString::fromLatin1(kPictureTranslate), t("Trans&late text from picture")},
        {QString::fromLatin1(kPicturePaste), t("Paste the &picture")},
    };
}

QList<Choice> resolve(const QString& setting, const QList<Item>& items)
{
    QSet<QString> known;
    for (const Item& item : items) {
        known.insert(item.id);
    }
    QList<Choice> out;
    QSet<QString> seen;
    for (QString token : setting.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        token = token.trimmed().toLower();
        const bool hidden = token.startsWith(QLatin1Char('-'));
        if (hidden) {
            token = token.mid(1).trimmed();
        }
        if (!known.contains(token) || seen.contains(token)) {
            continue;
        }
        seen.insert(token);
        out.append({token, !hidden});
    }
    for (const Item& item : items) {
        if (!seen.contains(item.id)) {
            out.append({item.id, true});
        }
    }
    return out;
}

QString serialize(const QList<Choice>& choices, const QList<Item>& items)
{
    QList<Choice> builtIn;
    for (const Item& item : items) {
        builtIn.append({item.id, true});
    }
    if (choices == builtIn) {
        return {};
    }
    QStringList parts;
    for (const Choice& c : choices) {
        parts << (c.shown ? c.id : QLatin1Char('-') + c.id);
    }
    return parts.join(QLatin1Char(','));
}

QStringList shownIds(const QString& setting, const QList<Item>& items)
{
    QStringList ids;
    for (const Choice& c : resolve(setting, items)) {
        if (c.shown) {
            ids << c.id;
        }
    }
    return ids;
}

QString plain(const QString& label)
{
    QString out;
    for (qsizetype i = 0; i < label.size(); ++i) {
        if (label[i] == QLatin1Char('&') && i + 1 < label.size()) {
            ++i; // "&&" is a literal &
        }
        out += label[i];
    }
    return out;
}

QStringList uniqueMnemonics(const QStringList& labels)
{
    QStringList out;
    QSet<QChar> used;
    for (const QString& label : labels) {
        QString fixed;
        for (qsizetype i = 0; i < label.size(); ++i) {
            if (label[i] == QLatin1Char('&') && i + 1 < label.size()) {
                const QChar next = label[i + 1];
                if (next == QLatin1Char('&')) {
                    fixed += QLatin1String("&&");
                } else if (!used.contains(next.toLower())) {
                    used.insert(next.toLower());
                    fixed += QLatin1Char('&');
                    fixed += next;
                } else {
                    fixed += next; // a repeated letter: no marker
                }
                ++i;
            } else {
                fixed += label[i];
            }
        }
        out << fixed;
    }
    return out;
}

} // namespace clipitto::pasteas
