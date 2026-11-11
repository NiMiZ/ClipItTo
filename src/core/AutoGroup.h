#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <unordered_map>
#include <vector>

namespace clipitto::autogroup {

// Local, model-free matching for "auto grouping" (roadmap 16): a copy is reduced to a few
// tokens (its words, and shapes such as a URL's host, a path's folder or a command's first
// word); a group is the set of tokens its examples had. Tokens are stored as 64-bit hashes,
// never as text (not readable at a glance, though a guessed word can be tested against it), and the copy's text is never kept or logged here.
struct Token {
    qint64 hash = 0;
    int weight = 1; // how much a match on this token is worth: shapes more than words
};

// Pure and cheap: only the first kMaxChars characters are read, at most kMaxTokens tokens returned.
std::vector<Token> tokens(const QString& text);

constexpr int kMaxChars = 4000;
constexpr int kMaxTokens = 48;
// What a match must reach before a copy is filed, and by how much it must beat the runner-up.
constexpr double kThreshold = 3.5;
constexpr double kMargin = 1.5;
// A correction (the user moved an automatically filed copy) counts this many examples.
constexpr int kCorrectionWeight = 3;

// An immutable snapshot of what was learned; safe to read from another thread.
class Model {
public:
    void add(const QString& group, qint64 token, int count);
    bool empty() const { return m_tokens.empty(); }
    int groupCount() const { return int(m_groups.size()); }
    // The best group for the text, or "" when no group clears the threshold or wins clearly.
    QString classify(const QString& text) const;
    // Same, from tokens already computed; `score` (optional) gets the best score.
    QString classify(const std::vector<Token>& tokens, double* score = nullptr) const;

private:
    struct Hit {
        int group;
        int count;
    };
    struct Entry {
        std::vector<Hit> hits;
        int total = 0;
    };
    QStringList m_groups;
    std::unordered_map<qint64, Entry> m_tokens;
};

} // namespace clipitto::autogroup
