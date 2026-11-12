#include "AutoForget.h"

#include "SecretScan.h"
#include "db/Repository.h"

#include <QStringList>

#include <algorithm>

namespace clipitto::autoforget {

namespace {

using Rule = AutoForgetRule;

bool plainText(const QString& text)
{
    for (const QChar ch : text) {
        if (ch.unicode() < 0x20 || ch == QLatin1Char('"') || ch.unicode() == 0x7f) {
            return false;
        }
    }
    return true;
}

// token / key / card, from the scanner's own name for what it found.
Rule::SecretKind classify(const QString& kind)
{
    if (kind == QLatin1String("card number")) {
        return Rule::SecretKind::Card;
    }
    if (kind.endsWith(QLatin1String("token"))) {
        return Rule::SecretKind::Token;
    }
    return Rule::SecretKind::Key; // private key, AWS/Google/Stripe/Anthropic/OpenAI key
}

bool parseMinutes(const QString& word, int& n)
{
    bool ok = false;
    if (word.size() < 2 || !word.endsWith(QLatin1Char('m'))) {
        return false;
    }
    n = word.chopped(1).toInt(&ok);
    return ok;
}

} // namespace

QString validate(const Rule& r)
{
    if (r.type == Rule::Type::Age) {
        if (r.amount < 1 || r.amount > kMaxDays) {
            return QStringLiteral("days must be between 1 and %1").arg(kMaxDays);
        }
        return {};
    }
    if (r.amount < 1 || r.amount > kMaxMinutes) {
        return QStringLiteral("minutes must be between 1 and %1").arg(kMaxMinutes);
    }
    if (r.type == Rule::Type::Contains) {
        if (r.text.size() < kMinTextLength || r.text.size() > kMaxTextLength) {
            return QStringLiteral("the text must be %1 to %2 characters").arg(kMinTextLength).arg(kMaxTextLength);
        }
        if (!plainText(r.text)) {
            return QStringLiteral("the text may not hold a double quote or a control character");
        }
    }
    return {};
}

QString format(const Rule& r)
{
    switch (r.type) {
    case Rule::Type::Age: {
        QString s = QStringLiteral("age %1d").arg(r.amount);
        if (r.scope == Rule::Scope::Text) {
            s += QStringLiteral(" text");
        } else if (r.scope == Rule::Scope::Picture) {
            s += QStringLiteral(" picture");
        }
        return s;
    }
    case Rule::Type::Contains:
        return QStringLiteral("match %1m contains \"%2\"").arg(r.amount).arg(r.text);
    case Rule::Type::Secret:
        break;
    }
    static const char* const names[] = {"secret", "token", "key", "card"};
    return QStringLiteral("match %1m %2").arg(r.amount).arg(QLatin1String(names[int(r.secret)]));
}

bool parse(const QString& line, Rule& out)
{
    const QString text = line.trimmed();
    Rule r;
    if (text.startsWith(QLatin1String("age "))) {
        const QStringList parts = text.mid(4).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool ok = false;
        if (parts.isEmpty() || parts.size() > 2 || !parts.first().endsWith(QLatin1Char('d'))) {
            return false;
        }
        r.amount = parts.first().chopped(1).toInt(&ok);
        if (!ok) {
            return false;
        }
        if (parts.size() == 2) {
            if (parts.at(1) == QLatin1String("text")) {
                r.scope = Rule::Scope::Text;
            } else if (parts.at(1) == QLatin1String("picture")) {
                r.scope = Rule::Scope::Picture;
            } else {
                return false;
            }
        }
    } else if (text.startsWith(QLatin1String("match "))) {
        const QString rest = text.mid(6).trimmed();
        const qsizetype space = rest.indexOf(QLatin1Char(' '));
        if (space < 0 || !parseMinutes(rest.left(space), r.amount)) {
            return false;
        }
        const QString what = rest.mid(space + 1).trimmed();
        if (what.startsWith(QLatin1String("contains "))) {
            const QString quoted = what.mid(9).trimmed();
            if (quoted.size() < 2 || !quoted.startsWith(QLatin1Char('"')) || !quoted.endsWith(QLatin1Char('"'))) {
                return false;
            }
            r.type = Rule::Type::Contains;
            r.text = quoted.mid(1, quoted.size() - 2);
        } else {
            r.type = Rule::Type::Secret;
            if (what == QLatin1String("secret")) {
                r.secret = Rule::SecretKind::Any;
            } else if (what == QLatin1String("token")) {
                r.secret = Rule::SecretKind::Token;
            } else if (what == QLatin1String("key")) {
                r.secret = Rule::SecretKind::Key;
            } else if (what == QLatin1String("card")) {
                r.secret = Rule::SecretKind::Card;
            } else {
                return false;
            }
        }
    } else {
        return false;
    }
    if (!validate(r).isEmpty()) {
        return false;
    }
    out = r;
    return true;
}

bool matchesText(const Rule& r, const QString& text)
{
    if (r.type == Rule::Type::Contains) {
        return !r.text.isEmpty() && text.contains(r.text, Qt::CaseInsensitive);
    }
    if (r.type == Rule::Type::Secret) {
        const QString kind = secretscan::kindOf(text);
        return !kind.isEmpty() && (r.secret == Rule::SecretKind::Any || classify(kind) == r.secret);
    }
    return false;
}

Pruner::Pruner(db::Repository& repo, Clock clock)
    : m_repo(repo)
    , m_clock(clock ? std::move(clock) : Clock([] { return QDateTime::currentDateTimeUtc(); }))
{
}

void Pruner::setRules(const QList<AutoForgetRule>& rules)
{
    if (rules != m_rules) {
        m_rules = rules;
        m_seen.clear();
    }
}

qint64 Pruner::run()
{
    m_more = false;
    if (m_rules.isEmpty()) {
        return 0;
    }
    const QDateTime now = m_clock().toUTC();
    std::vector<std::pair<QDateTime, db::Repository::ForgetKind>> cuts;
    int shortest = 0; // the shortest delay of a pattern rule
    for (const Rule& r : std::as_const(m_rules)) {
        if (r.type == Rule::Type::Age) {
            const auto kind = r.scope == Rule::Scope::Text ? db::Repository::ForgetKind::Text
                : r.scope == Rule::Scope::Picture          ? db::Repository::ForgetKind::Picture
                                                           : db::Repository::ForgetKind::Any;
            cuts.emplace_back(now.addDays(-r.amount), kind);
        } else {
            shortest = shortest == 0 ? r.amount : std::min(shortest, r.amount);
        }
    }

    std::vector<qint64> ids;
    if (shortest > 0) {
        const auto candidates = m_repo.forgetCandidates(now.addSecs(-qint64(shortest) * 60), kCandidateLimit);
        QHash<qint64, Seen> seen; // only what is still a candidate is kept
        qint64 budget = kScanBudgetBytes;
        int rows = kScanRows;
        for (const auto& c : candidates) {
            Seen entry;
            const auto known = m_seen.constFind(c.id);
            if (known != m_seen.constEnd() && known->hash == c.hash) {
                entry = *known;
            } else if (budget <= 0 || rows <= 0) {
                m_more = true;
                continue; // not looked at yet: the next run does
            } else {
                --rows;
                const QString text = m_repo.forgetText(c.id);
                budget -= text.size() * 2;
                entry.hash = c.hash;
                for (const Rule& r : std::as_const(m_rules)) {
                    if (r.type != Rule::Type::Age && matchesText(r, text)) {
                        entry.minutes = entry.minutes == 0 ? r.amount : std::min(entry.minutes, r.amount);
                    }
                }
            }
            seen.insert(c.id, entry);
            if (entry.minutes > 0 && c.lastUsed.isValid() && c.lastUsed < now.addSecs(-qint64(entry.minutes) * 60)) {
                ids.push_back(c.id);
            }
        }
        m_seen = seen;
    }
    if (cuts.empty() && ids.empty()) {
        return 0;
    }
    for (const qint64 id : ids) {
        m_seen.remove(id);
    }
    bool limited = false;
    const qint64 removed = m_repo.forget(cuts, ids, kDeleteLimit, &limited);
    m_more = m_more || limited;
    return removed;
}

} // namespace clipitto::autoforget
