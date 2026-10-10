#include "db/FtsQuery.h"

#include <algorithm>

namespace clipitto::db::fts {

QStringList tokenize(const QString& query)
{
    return query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

bool canUseTrigram(const QStringList& tokens)
{
    return !tokens.isEmpty() && std::all_of(tokens.begin(), tokens.end(), [](const QString& t) {
        return t.toUcs4().size() >= 3;
    });
}

QString matchExpression(const QStringList& tokens)
{
    QStringList phrases;
    phrases.reserve(tokens.size());
    for (QString token : tokens) {
        token.replace(QLatin1Char('"'), QLatin1String("\"\""));
        phrases << QLatin1Char('"') + token + QLatin1Char('"');
    }
    return phrases.join(QLatin1Char(' '));
}

QString likePattern(const QString& token)
{
    QString escaped;
    escaped.reserve(token.size() + 4);
    for (const QChar c : token) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('%') || c == QLatin1Char('_')) {
            escaped += QLatin1Char('\\');
        }
        escaped += c;
    }
    return QLatin1Char('%') + escaped + QLatin1Char('%');
}

} // namespace clipitto::db::fts
