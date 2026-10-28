#include "TextTransform.h"

#include <QCoreApplication>
#include <QHash>
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

} // namespace

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

// --- Keyboard layouts ---------------------------------------------------------------------------------------

namespace {

// Each layout lists, for the keys that give a different character than on the US-English keyboard, the
// English key's character (always lower case) and the layout's own. Keys that agree (digits, space, most
// symbols) pass through. Standard Windows layouts: Hebrew (SI-1452), Russian (ЙЦУКЕН), Greek (319).
struct KeyPair {
    char16_t key;
    char16_t own;
};

constexpr KeyPair kHebrew[] = {
    {u'q', u'/'}, {u'w', u'\''}, {u'e', u'ק'}, {u'r', u'ר'}, {u't', u'א'}, {u'y', u'ט'}, {u'u', u'ו'},
    {u'i', u'ן'}, {u'o', u'ם'}, {u'p', u'פ'}, {u'a', u'ש'}, {u's', u'ד'}, {u'd', u'ג'}, {u'f', u'כ'},
    {u'g', u'ע'}, {u'h', u'י'}, {u'j', u'ח'}, {u'k', u'ל'}, {u'l', u'ך'}, {u';', u'ף'}, {u'z', u'ז'},
    {u'x', u'ס'}, {u'c', u'ב'}, {u'v', u'ה'}, {u'b', u'נ'}, {u'n', u'מ'}, {u'm', u'צ'}, {u',', u'ת'},
    {u'.', u'ץ'}, {u'/', u'.'}, {u'\'', u','}, {u'`', u';'}, {u'[', u']'}, {u']', u'['},
    // Hebrew keyboards mirror the brackets so they read right in right-to-left text.
    {u'(', u')'}, {u')', u'('}, {u'{', u'}'}, {u'}', u'{'}, {u'<', u'>'}, {u'>', u'<'},
};

constexpr KeyPair kRussian[] = {
    {u'q', u'й'}, {u'w', u'ц'}, {u'e', u'у'}, {u'r', u'к'}, {u't', u'е'}, {u'y', u'н'}, {u'u', u'г'},
    {u'i', u'ш'}, {u'o', u'щ'}, {u'p', u'з'}, {u'[', u'х'}, {u']', u'ъ'}, {u'a', u'ф'}, {u's', u'ы'},
    {u'd', u'в'}, {u'f', u'а'}, {u'g', u'п'}, {u'h', u'р'}, {u'j', u'о'}, {u'k', u'л'}, {u'l', u'д'},
    {u';', u'ж'}, {u'\'', u'э'}, {u'z', u'я'}, {u'x', u'ч'}, {u'c', u'с'}, {u'v', u'м'}, {u'b', u'и'},
    {u'n', u'т'}, {u'm', u'ь'}, {u',', u'б'}, {u'.', u'ю'}, {u'`', u'ё'},
};

constexpr KeyPair kGreek[] = {
    {u'q', u';'}, {u'w', u'ς'}, {u'e', u'ε'}, {u'r', u'ρ'}, {u't', u'τ'}, {u'y', u'υ'}, {u'u', u'θ'},
    {u'i', u'ι'}, {u'o', u'ο'}, {u'p', u'π'}, {u'a', u'α'}, {u's', u'σ'}, {u'd', u'δ'}, {u'f', u'φ'},
    {u'g', u'γ'}, {u'h', u'η'}, {u'j', u'ξ'}, {u'k', u'κ'}, {u'l', u'λ'}, {u'z', u'ζ'}, {u'x', u'χ'},
    {u'c', u'ψ'}, {u'v', u'ω'}, {u'b', u'β'}, {u'n', u'ν'}, {u'm', u'μ'},
};

struct LayoutData {
    const char* id;
    const char* name;
    const KeyPair* keys;
    std::size_t count;
    char16_t firstLetter; // the layout's script, for telling which layout a text is typed in
    char16_t lastLetter;
};

template <std::size_t N>
constexpr LayoutData layout(const char* id, const char* name, const KeyPair (&keys)[N], char16_t first, char16_t last)
{
    return {id, name, keys, N, first, last};
}

const LayoutData& hebrew()
{
    static const LayoutData d = layout("he", "Hebrew", kHebrew, 0x0590, 0x05FF);
    return d;
}
const LayoutData& russian()
{
    static const LayoutData d = layout("ru", "Russian", kRussian, 0x0400, 0x04FF);
    return d;
}
const LayoutData& greek()
{
    static const LayoutData d = layout("el", "Greek", kGreek, 0x0370, 0x03FF);
    return d;
}

QList<const LayoutData*> allData()
{
    return {&hebrew(), &russian(), &greek()};
}

const LayoutData* find(const QString& id)
{
    for (const LayoutData* d : allData()) {
        if (id == QLatin1String(d->id)) {
            return d;
        }
    }
    return nullptr;
}

// The English key's character for `c` typed in `layout` (nullptr: English itself), lower case; 0 when `c`
// has no key there.
char16_t keyOf(const LayoutData* layout, QChar c)
{
    const QChar lower = c.toLower();
    if (!layout) {
        return lower.unicode();
    }
    for (std::size_t i = 0; i < layout->count; ++i) {
        if (layout->keys[i].own == lower.unicode()) {
            return layout->keys[i].key;
        }
    }
    return 0;
}

// The character `layout` gives on English key `key` (nullptr: English); 0 when it gives the same.
char16_t charOf(const LayoutData* layout, char16_t key)
{
    if (!layout) {
        return key;
    }
    for (std::size_t i = 0; i < layout->count; ++i) {
        if (layout->keys[i].key == key) {
            return layout->keys[i].own;
        }
    }
    return 0;
}

} // namespace

QList<Layout> layouts()
{
    QList<Layout> out;
    for (const LayoutData* d : allData()) {
        out.push_back({QString::fromLatin1(d->id), QString::fromLatin1(d->name)});
    }
    return out;
}

QString layoutName(const QString& id)
{
    if (id == QLatin1String("en")) {
        return t("English");
    }
    if (const LayoutData* d = find(id)) {
        return t(d->name);
    }
    return id;
}

QStringList enabledLayouts(const QString& setting, const QString& localeTag)
{
    const QString trimmed = setting.trimmed().toLower();
    QStringList out;
    if (trimmed == QLatin1String("none")) {
        return out;
    }
    if (trimmed.isEmpty()) {
        const QString language = localeTag.section(QRegularExpression(QStringLiteral("[-_]")), 0, 0).toLower();
        const QString id = language == QLatin1String("iw") ? QStringLiteral("he") : language; // an old code for Hebrew
        if (find(id)) {
            out << id;
        }
        return out;
    }
    for (const QString& part : trimmed.split(QRegularExpression(QStringLiteral("[,+\\s]+")), Qt::SkipEmptyParts)) {
        if (find(part) && !out.contains(part)) {
            out << part;
        }
    }
    return out;
}

QString layoutOf(const QString& text, const QStringList& enabled)
{
    int latin = 0;
    QHash<QString, int> counts;
    for (const QChar c : text) {
        if (!c.isLetter()) {
            continue;
        }
        if (c.unicode() < 0x80) {
            ++latin;
            continue;
        }
        for (const QString& id : enabled) {
            const LayoutData* d = find(id);
            if (d && c.unicode() >= d->firstLetter && c.unicode() <= d->lastLetter) {
                ++counts[id];
            }
        }
    }
    QString best = QStringLiteral("en");
    int most = latin;
    for (const QString& id : enabled) {
        if (counts.value(id) > most) {
            best = id;
            most = counts.value(id);
        }
    }
    return best;
}

QStringList convertibleTo(const QString& text, const QStringList& enabled)
{
    const QString own = layoutOf(text, enabled);
    QStringList out;
    if (own != QLatin1String("en")) {
        out << QStringLiteral("en");
    }
    for (const QString& id : enabled) {
        if (id != own && find(id) && !out.contains(id)) {
            out << id;
        }
    }
    return out;
}

std::optional<QString> convertLayout(const QString& text, const QString& from, const QString& to, QString* why)
{
    const bool fromEnglish = from == QLatin1String("en");
    const bool toEnglish = to == QLatin1String("en");
    const LayoutData* source = fromEnglish ? nullptr : find(from);
    const LayoutData* target = toEnglish ? nullptr : find(to);
    if ((!fromEnglish && !source) || (!toEnglish && !target) || from == to) {
        return fail(why, t("This conversion is not available."));
    }
    QString out;
    out.reserve(text.size());
    int changed = 0;
    for (const QChar original : text) {
        const char16_t key = keyOf(source, original);
        const char16_t converted = key ? charOf(target, key) : 0;
        if (!converted || converted == original.unicode()) {
            out.append(original);
            continue;
        }
        QChar result(converted);
        if (original.isUpper()) {
            result = result.toUpper(); // a no-op for scripts without capitals (Hebrew)
        }
        out.append(result);
        ++changed;
    }
    if (changed == 0) {
        return fail(why, t("There is nothing in the text to convert."));
    }
    return out;
}

} // namespace clipitto::texttransform
