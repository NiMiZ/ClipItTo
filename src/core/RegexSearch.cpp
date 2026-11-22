#include "RegexSearch.h"

#include <QCoreApplication>

namespace clipitto {

namespace {

// A match call on one text that takes this long without finding anything ran into the work limit.
constexpr qint64 kHeavyNs = 5'000'000;

QString tr(const char* text)
{
    return QCoreApplication::translate("clipitto::RegexQuery", text);
}

} // namespace

RegexQuery RegexQuery::parse(const QString& query)
{
    RegexQuery q;
    const qsizetype n = query.size();
    qsizetype i = 0;
    qsizetype tokenStart = -1;
    qsizetype tokenEnd = -1;
    QString pattern;
    int found = 0;
    while (i < n) {
        if (query.at(i).isSpace()) {
            ++i;
            continue;
        }
        const qsizetype start = i;
        const bool isRe = query.mid(i, 3).compare(QLatin1String("re:"), Qt::CaseInsensitive) == 0 && i + 3 < n
            && !query.at(i + 3).isSpace();
        if (isRe && query.at(i + 3) == QLatin1Char('"')) {
            const qsizetype close = query.indexOf(QLatin1Char('"'), i + 4);
            const qsizetype end = close < 0 ? n : close;
            if (found++ == 0) {
                pattern = query.mid(i + 4, end - (i + 4));
                tokenStart = start;
                tokenEnd = close < 0 ? n : close + 1;
            }
            i = close < 0 ? n : close + 1;
            continue;
        }
        while (i < n && !query.at(i).isSpace()) {
            ++i;
        }
        if (isRe && found++ == 0) {
            pattern = query.mid(start + 3, i - (start + 3));
            tokenStart = start;
            tokenEnd = i;
        }
    }
    if (found == 0) {
        return q;
    }
    q.present = true;
    q.pattern = pattern;
    q.rest = (query.left(tokenStart) + QLatin1Char(' ') + query.mid(tokenEnd)).simplified();
    if (found > 1) {
        q.error = tr("only one re: pattern at a time");
        return q;
    }
    if (pattern.isEmpty()) {
        q.error = tr("the pattern is empty");
        return q;
    }
    if (pattern.size() > kMaxPatternChars) {
        q.error = tr("too long (200 characters at most)");
        return q;
    }
    if (pattern.startsWith(QLatin1String("(*"))) {
        q.error = tr("(* at the start is not supported");
        return q;
    }
    // Case-insensitive unless the pattern says (?-i). NO_JIT: PCRE2's JIT does not count work, so it would ignore
    // the limit. The prefix is ours; a user pattern cannot come before it.
    const QString full = QStringLiteral("(*NO_JIT)(*LIMIT_MATCH=%1)").arg(kMatchLimit) + pattern;
    q.regex = QRegularExpression(full, QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    if (!q.regex.isValid()) {
        q.error = q.regex.errorString();
    }
    return q;
}

RegexScan::RegexScan(db::Repository& repo, const SearchFilter& filter, const QRegularExpression& regex, Options options)
    : m_repo(repo)
    , m_filter(filter)
    , m_regex(regex)
    , m_options(options)
{
}

RegexScan::RegexScan(db::Repository& repo, const SearchFilter& filter, const QRegularExpression& regex)
    : RegexScan(repo, filter, regex, Options())
{
}

bool RegexScan::matches(const Entry& entry)
{
    const auto test = [this](const QString& text) {
        if (text.isEmpty()) {
            return false;
        }
        const QString capped = text.size() > RegexQuery::kMaxScanChars ? text.left(RegexQuery::kMaxScanChars) : text;
        QElapsedTimer t;
        t.start();
        const bool hit = m_regex.match(capped).hasMatch();
        if (!hit && t.nsecsElapsed() >= kHeavyNs) {
            m_heavy = true; // the work limit ended the call, or the text was just too slow for the pattern
        }
        return hit;
    };
    if (test(entry.note) || test(entry.group)) {
        return true;
    }
    // A password copy's `content` is only the label "Password": it is not tried either, so the word cannot find them all.
    return !entry.secret && test(entry.content);
}

bool RegexScan::step()
{
    if (m_outcome != Outcome::Running) {
        return true;
    }
    QElapsedTimer slice;
    slice.start();
    if (!m_started) {
        m_started = true;
        m_clock.start();
        m_ids = m_repo.regexCandidateIds(m_filter); // one sort of the ids: the longest single piece of work
        return false;
    }
    while (true) {
        if (m_next >= m_ids.size()) {
            m_outcome = m_heavy ? Outcome::Stopped : Outcome::Ok;
            return true;
        }
        if (m_clock.elapsed() >= m_options.budgetMs) {
            m_outcome = Outcome::Stopped;
            return true;
        }
        if (slice.elapsed() >= m_options.sliceMs) {
            return false;
        }
        std::optional<Entry> e = m_repo.get(m_ids[m_next++]);
        if (!e) {
            continue; // deleted meanwhile
        }
        ++m_tried;
        if (matches(*e)) {
            m_results.push_back(std::move(*e));
            if (int(m_results.size()) >= m_options.resultLimit) {
                m_outcome = Outcome::Ok;
                return true;
            }
        }
    }
}

} // namespace clipitto
