#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

namespace clipitto {

// Filters typed in the search box next to ordinary words: "img:", "pin:", "star:", "text:",
// "group:work", "note:", "today:", "since:3d" ("30m", "2h", "3d", "2w", "yesterday", "2026-10-01").
// Everything else (also "http://x" or "foo:bar") is an ordinary word. A key alone ("img:") filters
// without a value; "group:" alone means "in any group" and "note:" "has a note". Values have no spaces.
struct SearchFilter {
    QStringList words; // the rest of the query
    bool images = false;
    bool textOnly = false;
    bool pinned = false;
    bool starred = false;
    std::optional<QString> group; // empty: any group
    std::optional<QString> note; // empty: has a note
    std::optional<QDateTime> since; // last copied or used at or after this (UTC)

    bool hasFilters() const { return images || textOnly || pinned || starred || group || note || since; }

    // `now` is the local time the relative values count from.
    static SearchFilter parse(const QString& query, const QDateTime& now = QDateTime::currentDateTime());
};

} // namespace clipitto
