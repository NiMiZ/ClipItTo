#pragma once

#include "Entry.h"
#include "SearchFilter.h"
#include "db/Repository.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QString>

#include <optional>
#include <vector>

namespace clipitto {

// Regex search: "re:<pattern>" in the search box, next to the ordinary filters ("group:work re:foo\d+").
//
// The pattern is one token like the other filters (no spaces: use \s or [ ]); put it in double quotes to
// type spaces: re:"error \d+ at". A key alone ("re:") is an ordinary word, so a searched "Re:" stays text.
// Only one pattern per query. The rest of the query (filters and words) is parsed by SearchFilter as usual and
// narrows the copies the pattern is tried on.
//
// Safety (Qt's QRegularExpression has no match time limit; PCRE2 can be told one inside the pattern):
//  - the pattern is compiled with "(*NO_JIT)(*LIMIT_MATCH=n)" in front, so one match call on one copy does a bounded
//    amount of work (about 10-20 ms of backtracking at most); a pattern that starts with "(*" itself is refused so it
//    cannot raise that limit;
//  - the pattern is at most kMaxPatternChars long and only the first kMaxScanChars characters of each text are tried;
//  - RegexScan works in short slices that the caller runs from the event loop, and stops for good after a total
//    budget (300 ms), keeping what it found so far; the next keystroke drops it.
// Passwords: only a password copy's note and group are tried, never its text. Never log a pattern or a match.
struct RegexQuery {
    static constexpr int kMaxPatternChars = 200;
    static constexpr int kMaxScanChars = 20000;
    static constexpr int kMatchLimit = 3000000; // PCRE2 match() calls per pcre2_match: ~15 ms in the worst case
    static constexpr int kBudgetMs = 300;
    static constexpr int kSliceMs = 15;

    bool present = false; // the query has a "re:" pattern
    QString pattern; // as typed (without "re:" and the quotes)
    QString rest; // the query without the "re:" token: ordinary filters and words
    QString error; // short reason when a present pattern cannot be used (also when it is too long)
    QRegularExpression regex; // valid when present and error is empty

    // Finds the "re:" token, checks and compiles it.
    static RegexQuery parse(const QString& query);
};

// Scans the history newest first, one slice at a time. Not thread-safe: use it on the thread of the Repository.
class RegexScan {
public:
    enum class Outcome {
        Running,
        Ok, // everything that passed the filters was tried (or the result limit was reached)
        Stopped, // the budget ran out, or a match hit the work limit: the result is what was found so far
    };

    struct Options {
        int resultLimit = 100;
        int budgetMs = RegexQuery::kBudgetMs;
        int sliceMs = RegexQuery::kSliceMs;
    };

    RegexScan(db::Repository& repo, const SearchFilter& filter, const QRegularExpression& regex, Options options);
    RegexScan(db::Repository& repo, const SearchFilter& filter, const QRegularExpression& regex);

    // Works for at most one slice. True when finished (outcome() is not Running).
    bool step();

    Outcome outcome() const { return m_outcome; }
    // Stopped, and a match call ran into the work limit: a pattern that backtracks too much, as opposed to a long list.
    bool patternTooHeavy() const { return m_heavy; }
    int copiesTried() const { return m_tried; }
    std::vector<Entry>& results() { return m_results; }
    int budgetMs() const { return m_options.budgetMs; }

    // True when `entry` matches: note and group, and the text unless it is a password copy.
    bool matches(const Entry& entry);

private:
    db::Repository& m_repo;
    SearchFilter m_filter;
    QRegularExpression m_regex;
    Options m_options;
    QElapsedTimer m_clock;
    bool m_started = false;
    Outcome m_outcome = Outcome::Running;
    bool m_heavy = false;
    int m_tried = 0;
    std::vector<Entry> m_results;
    std::vector<qint64> m_ids; // newest first, read when the scan starts
    std::size_t m_next = 0;
};

} // namespace clipitto
