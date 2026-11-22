#pragma once

#include <QString>

#include <array>

namespace clipitto::similar {

// "Show similar copies" (roadmap 22): which text copies count as the same as, or very close to, one copy.
//
// In one sentence: two texts are close when, after trimming, collapsing every run of white space (line breaks of any
// kind included) to one space and folding the case, they are equal, or their lengths differ by at most 10% and at most
// 10% of the longer text's characters would have to be inserted, deleted or changed to turn one into the other.
//
// Limits (the scan is bounded so the popup never stalls): the newest kMaxCopies text copies, the first kMaxChars
// characters of each, kBudgetMs of scanning in all. Passwords and pictures are never compared (the caller leaves them out).
inline constexpr int kMaxCopies = 5000;
inline constexpr int kMaxChars = 5000;
inline constexpr int kBudgetMs = 200;
inline constexpr double kMaxDifference = 0.10; // share of the longer text that may differ (length and edits)

// Cuts to kMaxChars, trims, collapses white space (\r\n, \r, \n, tabs, no-break spaces) to single spaces, case-folds.
QString normalise(const QString& text);

// The reference copy, prepared once so each comparison is cheap.
class Reference {
public:
    explicit Reference(const QString& text);
    bool isEmpty() const { return m_norm.isEmpty(); }
    // `text` is a copy's text, not yet normalised.
    bool matches(const QString& text) const;
    // Already normalised.
    bool matchesNormalised(const QString& norm) const;

private:
    static constexpr int kBuckets = 64;
    using Histogram = std::array<int, kBuckets>;
    static Histogram histogramOf(const QString& norm);
    QString m_norm;
    Histogram m_hist{};
};

// Edit distance of a and b when it is at most `limit`, else limit + 1 (banded, so it costs O(length x limit)).
int boundedDistance(const QString& a, const QString& b, int limit);

} // namespace clipitto::similar
