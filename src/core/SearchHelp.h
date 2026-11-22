#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace clipitto::searchhelp {

// What the search box understands, in ONE table: the "/?" help is generated from it, and tests check every
// row against the real parsers (SearchFilter, RegexQuery, SearchBar), so a new filter cannot leave the help stale.
// Adding a filter to SearchFilter::parse means adding its row in SearchHelp.cpp, or tst_searchfilter fails.
enum class Kind {
    Filter, // "img:" ... : SearchFilter::parse sets a filter for "<prefix>"
    Regex, // "re:" : RegexQuery::parse
    Password, // "pw:" and the configured keyword: SearchBar starts a hidden password search
    Rule, // quoting, case, limits, "/?" itself
};

struct Row {
    Kind kind;
    QString prefix; // what starts it: "img:", "re:", "pw:", ...
    QString example; // something to type
    QString meaning; // one line, translated
};

// The text that shows the help: exactly "/?", spaces around it ignored.
bool isHelpQuery(const QString& query);

// The password search prefix that always works ("pw:", then a space), besides the configured keyword.
QString passwordPrefix();

// The table, translated. `passwordKeyword` is the configured keyword (Settings, default "p[]"); it gets
// its own row unless it is the same as passwordPrefix().
std::vector<Row> rows(const QString& passwordKeyword);

// The prefixes of the rows of one kind, in table order (what the tests compare with the parsers).
QStringList prefixes(Kind kind, const QString& passwordKeyword = QString());

// The help as lines of text for the list's empty area: "<example>   <meaning>".
QString text(const QString& passwordKeyword);

} // namespace clipitto::searchhelp
