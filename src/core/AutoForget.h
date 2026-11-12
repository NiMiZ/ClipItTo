#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include <functional>

namespace clipitto::db {
class Repository;
}

namespace clipitto {

// A rule for copies that remove themselves (Settings > Passwords > Auto-forget, `auto_forget`; roadmap 18).
//   Age:      copies last used more than `amount` days ago (optionally only text or only pictures).
//   Contains: text copies that contain `text` (any letter case) are removed `amount` minutes after they were copied.
//   Secret:   text copies that the secret scanner takes for a token, key or card number, after `amount` minutes.
// Pinned, starred, grouped and password copies are never removed by any rule. Matching by the app a copy came
// from is not possible yet: ClipItTo stores the source app only for password copies (roadmap 21).
struct AutoForgetRule {
    enum class Type { Age, Contains, Secret };
    enum class Scope { Any, Text, Picture }; // Age only
    enum class SecretKind { Any, Token, Key, Card }; // Secret only

    Type type = Type::Age;
    int amount = 30; // days (Age) or minutes (Contains, Secret)
    Scope scope = Scope::Any;
    SecretKind secret = SecretKind::Any;
    QString text; // Contains

    bool operator==(const AutoForgetRule&) const = default;
};

namespace autoforget {

constexpr int kMaxRules = 8;
constexpr int kMaxDays = 3650;
constexpr int kMaxMinutes = 10080; // a week
constexpr int kMinTextLength = 3; // so a rule can never catch nearly every copy
constexpr int kMaxTextLength = 100;

// Why a rule cannot be used, empty when it is fine.
QString validate(const AutoForgetRule& rule);

// Config text form, one string per rule:
//   age 30d            age 1d picture          age 7d text
//   match 5m contains "Bearer "                (the text is in double quotes, no quote or line break inside)
//   match 10m token    match 10m key    match 10m card    match 10m secret
// parse() returns false (and leaves `out` alone) for anything malformed or invalid.
QString format(const AutoForgetRule& rule);
bool parse(const QString& text, AutoForgetRule& out);

// Does a text copy fit a Contains or Secret rule? (Age rules match by time only: false here.) Never logs the text.
bool matchesText(const AutoForgetRule& rule, const QString& text);

// Runs the rules against the history. Cheap enough for the GUI thread: age rules are one indexed DELETE each;
// pattern rules read only copies not looked at before (what was found not to match is remembered as a short
// hash, not as text), at most kScanBudgetBytes of text and kScanRows copies per run, the rest on the next run.
class Pruner {
public:
    using Clock = std::function<QDateTime()>;
    static constexpr qint64 kScanBudgetBytes = 4 * 1024 * 1024;
    static constexpr int kScanRows = 500;
    static constexpr int kCandidateLimit = 50000;
    static constexpr int kDeleteLimit = 100; // copies removed per run; a bigger backlog goes on in the next run

    explicit Pruner(db::Repository& repo, Clock clock = {});

    void setRules(const QList<AutoForgetRule>& rules);
    const QList<AutoForgetRule>& rules() const { return m_rules; }

    // Removes what the rules say is due. Returns how many copies went (0 with no rules, without touching the database).
    // May throw db::DbError.
    qint64 run();
    // The last run stopped at its limit: more may be due now, so run again soon (not in the same event loop turn).
    bool hasMore() const { return m_more; }

private:
    struct Seen {
        QString hash;
        int minutes = 0; // the shortest delay of a pattern rule that matched; 0 = none matched
    };
    db::Repository& m_repo;
    Clock m_clock;
    QList<AutoForgetRule> m_rules;
    QHash<qint64, Seen> m_seen; // reset when the rules change
    bool m_more = false;
};

} // namespace autoforget
} // namespace clipitto
