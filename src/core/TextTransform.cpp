#include "TextTransform.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

namespace clipitto::texttransform {

namespace {

QString t(const char* text)
{
    return QCoreApplication::translate("clipitto::texttransform", text);
}

std::optional<QString> fail(QString* why, const QString& reason)
{
    if (why) {
        *why = reason;
    }
    return std::nullopt;
}

QString title(const QString& text)
{
    QString out = text.toLower();
    bool start = true;
    for (QChar& c : out) {
        if (c.isLetter()) {
            if (start) {
                c = c.toUpper();
            }
            start = false;
        } else if (!c.isDigit() && c != QLatin1Char('\'')) {
            start = true;
        } else {
            start = false;
        }
    }
    return out;
}

// The standard US-English and Hebrew (SI-1452) layouts: each physical key's character in both. Keys that
// give the same character in both (digits, space, most symbols) are left out and pass through. Shift on
// a letter key gives the same Hebrew letter, so an upper-case Latin letter converts like its lower case.
struct KeyPair {
    char16_t latin;
    char16_t hebrew;
};
constexpr KeyPair kKeys[] = {
    {u'q', u'/'}, {u'w', u'\''}, {u'e', u'ק'}, {u'r', u'ר'}, {u't', u'א'}, {u'y', u'ט'}, {u'u', u'ו'},
    {u'i', u'ן'}, {u'o', u'ם'}, {u'p', u'פ'}, {u'a', u'ש'}, {u's', u'ד'}, {u'd', u'ג'}, {u'f', u'כ'},
    {u'g', u'ע'}, {u'h', u'י'}, {u'j', u'ח'}, {u'k', u'ל'}, {u'l', u'ך'}, {u';', u'ף'}, {u'z', u'ז'},
    {u'x', u'ס'}, {u'c', u'ב'}, {u'v', u'ה'}, {u'b', u'נ'}, {u'n', u'מ'}, {u'm', u'צ'}, {u',', u'ת'},
    {u'.', u'ץ'}, {u'/', u'.'}, {u'\'', u','}, {u'`', u';'}, {u'[', u']'}, {u']', u'['},
    // Hebrew keyboards mirror the brackets so they read right in right-to-left text.
    {u'(', u')'}, {u')', u'('}, {u'{', u'}'}, {u'}', u'{'}, {u'<', u'>'}, {u'>', u'<'},
};

bool isHebrewLetter(QChar c)
{
    return c.unicode() >= 0x05D0 && c.unicode() <= 0x05EA;
}

// Converts every character that has a key in the table; others stay. Nothing converted: nullopt.
std::optional<QString> convertKeys(const QString& text, bool toHebrew, QString* why)
{
    QString out;
    out.reserve(text.size());
    int changed = 0;
    for (const QChar original : text) {
        QChar c = original;
        if (toHebrew && c.isUpper()) {
            c = c.toLower();
        }
        bool found = false;
        for (const KeyPair& k : kKeys) {
            const char16_t from = toHebrew ? k.latin : k.hebrew;
            const char16_t to = toHebrew ? k.hebrew : k.latin;
            // Punctuation that is the same on both layouts' other side would be ambiguous: only letters
            // decide which direction a text goes (see fits()), the rest follows the table.
            if (c.unicode() == from) {
                out.append(QChar(to));
                ++changed;
                found = true;
                break;
            }
        }
        if (!found) {
            out.append(original);
        }
    }
    if (changed == 0) {
        return fail(why, t("There is nothing in the text to convert."));
    }
    return out;
}

} // namespace

bool fits(Kind kind, const QString& text)
{
    if (kind != Kind::HebrewKeyboard && kind != Kind::EnglishKeyboard) {
        return true;
    }
    bool latin = false;
    bool hebrew = false;
    for (const QChar c : text) {
        latin = latin || (c.unicode() < 0x80 && c.isLetter());
        hebrew = hebrew || isHebrewLetter(c);
    }
    // Hebrew letters and no Latin ones: it was typed on the Hebrew keys. Latin and no Hebrew: the English ones.
    // Both kinds of letters: either might be meant.
    return kind == Kind::HebrewKeyboard ? (latin || !hebrew) : (hebrew || !latin);
}

QList<Info> all()
{
    return {
        {Kind::Upper, t("&UPPER CASE")},
        {Kind::Lower, t("&lower case")},
        {Kind::Title, t("&Title Case")},
        {Kind::Trim, t("Tri&m white space at the ends")},
        {Kind::CollapseSpaces, t("Collapse &spaces")},
        {Kind::OneLine, t("&One line")},
        {Kind::JsonPretty, t("&JSON, pretty")},
        {Kind::JsonMinify, t("JSON, m&inified")},
        {Kind::UrlEncode, t("URL-&encode")},
        {Kind::UrlDecode, t("URL-&decode")},
        {Kind::Base64Encode, t("&Base64-encode")},
        {Kind::Base64Decode, t("Base64-de&code")},
        {Kind::HebrewKeyboard, t("&Hebrew keyboard (xccv → סבבה)")},
        {Kind::EnglishKeyboard, t("English &keyboard (סבבה → xccv)")},
    };
}

std::optional<QString> apply(Kind kind, const QString& text, QString* why)
{
    switch (kind) {
    case Kind::Upper:
        return text.toUpper();
    case Kind::Lower:
        return text.toLower();
    case Kind::Title:
        return title(text);
    case Kind::Trim:
        return text.trimmed();
    case Kind::CollapseSpaces: {
        static const QRegularExpression runs(QStringLiteral("[ \\t]+"));
        QString out = text;
        out.replace(runs, QStringLiteral(" "));
        return out;
    }
    case Kind::OneLine: {
        static const QRegularExpression breaks(QStringLiteral("\\s*[\\r\\n]+\\s*"));
        QString out = text.trimmed();
        out.replace(breaks, QStringLiteral(" "));
        return out;
    }
    case Kind::JsonPretty:
    case Kind::JsonMinify: {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError) {
            return fail(why, t("Not valid JSON: %1").arg(error.errorString()));
        }
        const auto format = kind == Kind::JsonPretty ? QJsonDocument::Indented : QJsonDocument::Compact;
        return QString::fromUtf8(doc.toJson(format)).trimmed();
    }
    case Kind::UrlEncode:
        return QString::fromLatin1(QUrl::toPercentEncoding(text));
    case Kind::UrlDecode: {
        const QByteArray bytes = QByteArray::fromPercentEncoding(text.toUtf8());
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder(bytes);
        if (decoder.hasError()) {
            return fail(why, t("The decoded bytes are not valid text."));
        }
        return decoded;
    }
    case Kind::Base64Encode:
        return QString::fromLatin1(text.toUtf8().toBase64());
    case Kind::HebrewKeyboard:
        return convertKeys(text, /*toHebrew=*/true, why);
    case Kind::EnglishKeyboard:
        return convertKeys(text, /*toHebrew=*/false, why);
    case Kind::Base64Decode: {
        const auto decoded = QByteArray::fromBase64Encoding(text.trimmed().toUtf8(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            return fail(why, t("Not valid Base64."));
        }
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString out = decoder(decoded.decoded);
        if (decoder.hasError()) {
            return fail(why, t("The decoded bytes are not valid text."));
        }
        return out;
    }
    }
    return fail(why, QString());
}

} // namespace clipitto::texttransform
