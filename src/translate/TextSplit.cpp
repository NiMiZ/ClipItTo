#include "Translate.h"

#include <QHash>

namespace clipitto::translate {

namespace {

QString scriptOfChar(QChar c)
{
    const char32_t u = c.unicode();
    if (!c.isLetter()) {
        return {};
    }
    if (u < 0x250) {
        return QStringLiteral("latin");
    }
    if (u >= 0x0370 && u <= 0x03FF) {
        return QStringLiteral("greek");
    }
    if (u >= 0x0400 && u <= 0x052F) {
        return QStringLiteral("cyrillic");
    }
    if (u >= 0x0590 && u <= 0x05FF) {
        return QStringLiteral("hebrew");
    }
    if ((u >= 0x0600 && u <= 0x06FF) || (u >= 0x0750 && u <= 0x077F)) {
        return QStringLiteral("arabic");
    }
    if (u >= 0x0900 && u <= 0x097F) {
        return QStringLiteral("devanagari");
    }
    if ((u >= 0x3040 && u <= 0x30FF)) {
        return QStringLiteral("kana");
    }
    if (u >= 0xAC00 && u <= 0xD7AF) {
        return QStringLiteral("hangul");
    }
    if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3400 && u <= 0x4DBF)) {
        return QStringLiteral("han");
    }
    if (u >= 0x1E00 && u <= 0x1EFF) {
        return QStringLiteral("latin");
    }
    return {};
}

} // namespace

QString scriptOfLanguage(const QString& code)
{
    static const QHash<QString, QString> scripts = {
        {QStringLiteral("zh"), QStringLiteral("han")},      {QStringLiteral("zt"), QStringLiteral("han")},
        {QStringLiteral("ja"), QStringLiteral("kana")},     {QStringLiteral("ko"), QStringLiteral("hangul")},
        {QStringLiteral("he"), QStringLiteral("hebrew")},   {QStringLiteral("ru"), QStringLiteral("cyrillic")},
        {QStringLiteral("uk"), QStringLiteral("cyrillic")}, {QStringLiteral("bg"), QStringLiteral("cyrillic")},
        {QStringLiteral("el"), QStringLiteral("greek")},    {QStringLiteral("ar"), QStringLiteral("arabic")},
        {QStringLiteral("fa"), QStringLiteral("arabic")},   {QStringLiteral("ur"), QStringLiteral("arabic")},
        {QStringLiteral("hi"), QStringLiteral("devanagari")},
    };
    return scripts.value(code.section(QLatin1Char('-'), 0, 0).toLower(), QStringLiteral("latin"));
}

QString scriptOfText(const QString& text)
{
    // The first part decides: this runs on the GUI thread (menus, Ctrl+T) and a copy can be megabytes long
    // (a 5 MB text took 450 ms to scan).
    constexpr qsizetype kLookAt = 20000;
    QHash<QString, int> counts;
    for (const QChar c : QStringView(text).left(kLookAt)) {
        const QString script = scriptOfChar(c);
        if (!script.isEmpty()) {
            ++counts[script];
        }
    }
    // Japanese text mixes kana with Han characters: kana decides.
    if (counts.value(QStringLiteral("kana")) > 0) {
        return QStringLiteral("kana");
    }
    QString best;
    int most = 0;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        if (it.value() > most) {
            best = it.key();
            most = it.value();
        }
    }
    return best;
}

bool fits(const Pack& pack, const QString& text)
{
    const QString script = scriptOfText(text);
    return !script.isEmpty() && scriptOfLanguage(pack.from) == script;
}

QList<Piece> splitSentences(const QString& text)
{
    QList<Piece> out;
    const auto isStop = [](QChar c) {
        return c == u'。' || c == u'！' || c == u'？' || c == u'!' || c == u'?' || c == u'.' || c == u'؟' || c == u'।';
    };
    qsizetype i = 0;
    const qsizetype n = text.size();
    while (i < n) {
        // One line at a time; the line break(s) after it are its separator.
        qsizetype lineEnd = i;
        while (lineEnd < n && text[lineEnd] != u'\n' && text[lineEnd] != u'\r') {
            ++lineEnd;
        }
        qsizetype breakEnd = lineEnd;
        while (breakEnd < n && (text[breakEnd] == u'\n' || text[breakEnd] == u'\r')) {
            ++breakEnd;
        }
        const QString line = text.mid(i, lineEnd - i);
        // Sentences inside the line: after a stop (and the closing quotes / spaces that follow it).
        qsizetype start = 0;
        qsizetype pos = 0;
        QList<Piece> parts;
        while (pos < line.size()) {
            if (isStop(line[pos])) {
                qsizetype end = pos + 1;
                while (end < line.size() && (isStop(line[end]) || line[end] == u'"' || line[end] == u'\'' || line[end] == u'”'
                                             || line[end] == u'」' || line[end] == u'』' || line[end] == u')' || line[end] == u'）')) {
                    ++end;
                }
                const bool cjkStop = line[pos] == u'。' || line[pos] == u'！' || line[pos] == u'？';
                // A Latin stop ends a sentence only before a space (not "3.14" or "a.b").
                if (cjkStop || end >= line.size() || line[end].isSpace()) {
                    qsizetype sep = end;
                    while (sep < line.size() && line[sep].isSpace()) {
                        ++sep;
                    }
                    parts.push_back({line.mid(start, end - start), line.mid(end, sep - end)});
                    start = sep;
                    pos = sep;
                    continue;
                }
            }
            ++pos;
        }
        if (start < line.size()) {
            parts.push_back({line.mid(start), QString()});
        }
        if (parts.isEmpty()) {
            parts.push_back({QString(), QString()});
        }
        parts.back().after += text.mid(lineEnd, breakEnd - lineEnd);
        out.append(parts);
        i = breakEnd;
    }
    return out;
}

bool worthTranslating(const QString& piece)
{
    const QString t = piece.trimmed();
    bool letter = false;
    for (const QChar c : t) {
        if (c.isLetter()) {
            letter = true;
            break;
        }
    }
    if (!letter) {
        return false; // numbers, "---", "12:30", "#4711"
    }
    // One token that is a web address or an e-mail address: the model would only mangle it.
    if (!t.contains(QLatin1Char(' ')) && !t.contains(QLatin1Char('\t'))
        && (t.contains(QStringLiteral("://")) || t.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)
            || (t.contains(QLatin1Char('@')) && t.contains(QLatin1Char('.'))))) {
        return false;
    }
    return true;
}

QString assemble(const QList<Piece>& pieces, const QStringList& translated)
{
    QString out;
    for (qsizetype i = 0; i < pieces.size(); ++i) {
        const QString& original = pieces[i].text;
        const QString mine = i < translated.size() ? translated[i] : QString();
        if (mine.isEmpty()) {
            out += original; // not translated (nothing to say, a number, a link): as it was
        } else {
            qsizetype lead = 0;
            while (lead < original.size() && original[lead].isSpace()) {
                ++lead;
            }
            qsizetype trail = 0;
            while (trail < original.size() - lead && original[original.size() - 1 - trail].isSpace()) {
                ++trail;
            }
            out += original.left(lead) + mine + original.right(trail); // indentation and end spaces stay
        }
        out += pieces[i].after;
    }
    return out;
}

} // namespace clipitto::translate
