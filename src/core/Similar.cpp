#include "Similar.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace clipitto::similar {

QString normalise(const QString& text)
{
    const QString capped = text.size() > kMaxChars ? text.left(kMaxChars) : text;
    QString out;
    out.reserve(capped.size());
    bool pendingSpace = false;
    for (const QChar c : capped) {
        if (c.isSpace() || c == QChar(0x200B) /* zero-width space */) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) {
            out += QLatin1Char(' ');
            pendingSpace = false;
        }
        out += c;
    }
    return out.toCaseFolded();
}

int boundedDistance(const QString& a, const QString& b, int limit)
{
    const int n = int(a.size());
    const int m = int(b.size());
    if (std::abs(n - m) > limit) {
        return limit + 1;
    }
    // Equal beginnings and endings cost nothing: near copies are mostly that.
    int start = 0;
    while (start < n && start < m && a[start] == b[start]) {
        ++start;
    }
    int endA = n;
    int endB = m;
    while (endA > start && endB > start && a[endA - 1] == b[endB - 1]) {
        --endA;
        --endB;
    }
    const int len1 = endA - start;
    const int len2 = endB - start;
    if (len1 == 0 || len2 == 0) {
        const int rest = std::max(len1, len2);
        return rest > limit ? limit + 1 : rest;
    }
    const int inf = limit + 1;
    const QChar* const pa = a.constData() + start;
    const QChar* const pb = b.constData() + start;
    std::vector<int> prev(len2 + 1, inf);
    std::vector<int> cur(len2 + 1, inf);
    for (int j = 0; j <= std::min(len2, limit); ++j) {
        prev[j] = j;
    }
    for (int i = 1; i <= len1; ++i) {
        const int lo = std::max(1, i - limit);
        const int hi = std::min(len2, i + limit);
        std::fill(cur.begin(), cur.end(), inf);
        if (i <= limit) {
            cur[0] = i;
        }
        int rowMin = cur[0];
        for (int j = lo; j <= hi; ++j) {
            const int cost = pa[i - 1] == pb[j - 1] ? 0 : 1;
            int v = prev[j - 1] + cost;
            v = std::min(v, prev[j] + 1);
            v = std::min(v, cur[j - 1] + 1);
            cur[j] = std::min(v, inf);
            rowMin = std::min(rowMin, cur[j]);
        }
        if (rowMin > limit) {
            return limit + 1;
        }
        std::swap(prev, cur);
    }
    return std::min(prev[len2], inf);
}

Reference::Histogram Reference::histogramOf(const QString& norm)
{
    Histogram h{};
    for (const QChar c : norm) {
        ++h[c.unicode() % kBuckets];
    }
    return h;
}

Reference::Reference(const QString& text)
    : m_norm(normalise(text))
    , m_hist(histogramOf(m_norm))
{
}

bool Reference::matches(const QString& text) const
{
    return matchesNormalised(normalise(text));
}

bool Reference::matchesNormalised(const QString& norm) const
{
    if (m_norm.isEmpty() || norm.isEmpty()) {
        return false;
    }
    if (norm == m_norm) {
        return true;
    }
    const int longer = int(std::max(norm.size(), m_norm.size()));
    const int limit = int(longer * kMaxDifference);
    if (std::abs(int(norm.size() - m_norm.size())) > limit) {
        return false;
    }
    // Every edit changes at most two buckets by one: a cheap lower bound that rejects most strangers.
    const Histogram h = histogramOf(norm);
    int diff = 0;
    for (int i = 0; i < kBuckets; ++i) {
        diff += std::abs(h[i] - m_hist[i]);
    }
    if ((diff + 1) / 2 > limit) {
        return false;
    }
    return boundedDistance(m_norm, norm, limit) <= limit;
}

} // namespace clipitto::similar
