#include "SearchFilter.h"

#include <QRegularExpression>
#include <QTimeZone>

namespace clipitto {

namespace {

// "30m", "2h", "3d", "2w", "today", "yesterday" or an ISO date; nullopt when it is none of these.
std::optional<QDateTime> parseSince(const QString& value, const QDateTime& now)
{
    const QString v = value.toLower();
    if (v.isEmpty() || v == QLatin1String("today")) {
        return QDateTime(now.date(), QTime(0, 0), now.timeZone());
    }
    if (v == QLatin1String("yesterday")) {
        return QDateTime(now.date().addDays(-1), QTime(0, 0), now.timeZone());
    }
    static const QRegularExpression relative(QStringLiteral("^(\\d{1,4})([mhdw])$"));
    const QRegularExpressionMatch m = relative.match(v);
    if (m.hasMatch()) {
        const int n = m.captured(1).toInt();
        switch (m.captured(2).at(0).unicode()) {
        case u'm':
            return now.addSecs(-60LL * n);
        case u'h':
            return now.addSecs(-3600LL * n);
        case u'd':
            return now.addDays(-n);
        default:
            return now.addDays(-7 * n);
        }
    }
    const QDate date = QDate::fromString(v, Qt::ISODate);
    if (date.isValid()) {
        return QDateTime(date, QTime(0, 0), now.timeZone());
    }
    return std::nullopt;
}

} // namespace

SearchFilter SearchFilter::parse(const QString& query, const QDateTime& now)
{
    SearchFilter f;
    for (const QString& token : query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        const qsizetype colon = token.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            f.words << token;
            continue;
        }
        const QString key = token.left(colon).toLower();
        const QString value = token.mid(colon + 1);
        if (value.contains(QLatin1Char('/')) && value.startsWith(QLatin1String("//"))) {
            f.words << token; // a URL
            continue;
        }
        if ((key == QLatin1String("img") || key == QLatin1String("image")) && value.isEmpty()) {
            f.images = true;
        } else if (key == QLatin1String("text") && value.isEmpty()) {
            f.textOnly = true;
        } else if (key == QLatin1String("pin") && value.isEmpty()) {
            f.pinned = true;
        } else if (key == QLatin1String("star") && value.isEmpty()) {
            f.starred = true;
        } else if (key == QLatin1String("group")) {
            f.group = value;
        } else if (key == QLatin1String("note")) {
            f.note = value;
        } else if (key == QLatin1String("today") && value.isEmpty()) {
            f.since = parseSince(QString(), now);
        } else if (key == QLatin1String("since")) {
            if (const auto since = parseSince(value, now)) {
                f.since = since;
            } else {
                f.words << token; // "since:soon" is just text
            }
        } else {
            f.words << token;
        }
    }
    if (f.since) {
        f.since = f.since->toUTC();
    }
    return f;
}

} // namespace clipitto
