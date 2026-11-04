#include "RichText.h"

#include <QRegularExpression>
#include <QSet>

namespace clipitto::richtext {

namespace {

const QSet<QString>& formattingTags()
{
    static const QSet<QString> tags{
        QStringLiteral("a"),      QStringLiteral("b"),      QStringLiteral("strong"), QStringLiteral("i"),
        QStringLiteral("em"),     QStringLiteral("u"),      QStringLiteral("s"),      QStringLiteral("strike"),
        QStringLiteral("del"),    QStringLiteral("ins"),    QStringLiteral("mark"),   QStringLiteral("sub"),
        QStringLiteral("sup"),    QStringLiteral("small"),  QStringLiteral("big"),    QStringLiteral("code"),
        QStringLiteral("pre"),    QStringLiteral("h1"),     QStringLiteral("h2"),     QStringLiteral("h3"),
        QStringLiteral("h4"),     QStringLiteral("h5"),     QStringLiteral("h6"),     QStringLiteral("ul"),
        QStringLiteral("ol"),     QStringLiteral("li"),     QStringLiteral("table"),  QStringLiteral("blockquote"),
        QStringLiteral("img"),
    };
    return tags;
}

} // namespace

bool hasFormatting(const QString& html)
{
    static const QRegularExpression tag(QStringLiteral(R"(<\s*([a-zA-Z][a-zA-Z0-9]*)([^>]*)>)"));
    // Styles that change how the text looks (a colour or a font family alone does not count).
    static const QRegularExpression style(
        QStringLiteral(R"(style\s*=\s*["'][^"']*(font-weight|font-style|text-decoration|background)[^"']*["'])"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = tag.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (formattingTags().contains(m.captured(1).toLower())) {
            return true;
        }
        if (style.match(m.captured(2)).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool matchesText(const QString& html, const QString& text)
{
    static const QRegularExpression tags(QStringLiteral(R"(<[^>]*>)"));
    QString visible = html;
    visible.remove(tags);
    visible.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "))
        .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
        .replace(QStringLiteral("&gt;"), QStringLiteral(">"))
        .replace(QStringLiteral("&quot;"), QStringLiteral("\""))
        .replace(QStringLiteral("&#39;"), QStringLiteral("'"))
        .replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    int checked = 0;
    for (const QString& word : text.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts)) {
        if (word.size() < 3) {
            continue; // bullets, numbers and the like
        }
        if (!visible.contains(word)) {
            return false;
        }
        if (++checked == 20) {
            break;
        }
    }
    return true;
}

} // namespace clipitto::richtext
