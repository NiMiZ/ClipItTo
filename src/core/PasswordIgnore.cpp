#include "PasswordIgnore.h"

#include <QRegularExpression>

#include <vector>

namespace clipitto::passwordignore {

namespace {

struct Shape {
    const char* name;
    const char* pattern; // anchored by hand; matched against a single line
    bool caseless;
};

// One table, in the order of the checks. Every pattern is anchored and conservative: a generated password mixes
// symbols into its words, so it does not have the shape of a path or a variable. Unix variables are the upper case
// names that really occur ($HOME, $XDG_DATA_HOME), so "$aB3xY9qL" stays a password; ${NAME} and $env:NAME are
// unambiguous and take any letter case.
const Shape kShapes[] = {
    // Windows %NAME% at the start (alone or before a separator), or inside a path right after a separator.
    {"environment variable", R"(\A%[A-Za-z_][A-Za-z0-9_()]+%(?:[\\/][A-Za-z0-9._ \-()\\/%~+,]*)?\z)", false},
    {"environment variable", R"([\\/]%[A-Za-z_][A-Za-z0-9_()]+%[\\/])", false},
    // Unix $NAME (upper case: up to 8 letters, or with an underscore), ${NAME}, PowerShell $env:NAME.
    {"environment variable", R"(\A\$(?:[A-Z]{1,8}|[A-Z]+(?:_[A-Z0-9]+)+|_[A-Z0-9_]*)(?:[\\/][A-Za-z0-9._ \-()\\/%~+,]*)?\z)", false},
    {"environment variable", R"(\A\$\{[A-Za-z_][A-Za-z0-9_]*\}(?:[\\/][A-Za-z0-9._ \-()\\/%~+,]*)?\z)", false},
    {"environment variable", R"(\A\$env:[A-Za-z_][A-Za-z0-9_]*(?:[\\/][A-Za-z0-9._ \-()\\/%~+,]*)?\z)", true},
    // Home directory: ~ alone, ~/x, ~\x, ~user/x (not "~user" alone: that can be the start of a password).
    {"environment variable", R"(\A~\z)", false},
    {"environment variable", R"(\A~(?:[A-Za-z_][A-Za-z0-9_.-]*)?[\\/][A-Za-z0-9._ \-()\\/%~+,]*\z)", false},

    // Windows drive path, UNC path.
    {"file path", R"(\A[A-Za-z]:[\\/][A-Za-z0-9._ \-()\\/%~+,]*\z)", false},
    {"file path", R"(\A\\\\[^\\/\s]+\\[A-Za-z0-9._ \-()%~+,$][A-Za-z0-9._ \-()\\/%~+,$]*\z)", false},
    // Unix: well-known roots, or three plain segments, or two with a dot.
    {"file path", R"(\A/(?:usr|etc|home|var|opt|mnt|tmp|bin|sbin|lib|lib64|dev|proc|sys|srv|root|run|boot|media|snap|nix|cores|Users|Applications|Library|Volumes|System|Network|private)(?:/\S*)?\z)", false},
    {"file path", R"(\A/[A-Za-z0-9._ -]+(?:/[A-Za-z0-9._ -]+){2,}/?\z)", false},
    {"file path", R"(\A/[A-Za-z0-9._ -]+/[A-Za-z0-9_ -]*\.[A-Za-z0-9._ -]+\z)", false},
    // Relative.
    {"file path", R"(\A\.{1,2}[\\/][A-Za-z0-9._ \-()\\/%~+,]*\z)", false},

    {"registry key",
     R"(\A(?:HKEY_(?:LOCAL_MACHINE|CURRENT_USER|CLASSES_ROOT|USERS|CURRENT_CONFIG)|HKLM|HKCU|HKCR|HKU|HKCC)(?:\\[A-Za-z0-9._ \-()\\/%~+,]*)?\z)",
     false},

    {"host and port", R"(\A(?:localhost|[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+):\d{1,5}(?:/\S*)?\z)", true},
    {"host and port", R"(\Alocalhost(?:/\S*)?\z)", true},
    // Reverse-DNS (com.example.Thing) or a well-known namespace; a three-word passphrase is not one.
    {"dotted name", R"(\A(?:[a-z]{2,}(?:\.[A-Za-z]{2,}){2,}|(?:System|Microsoft|Windows|UnityEngine)(?:\.[A-Za-z]{2,}){2,})\z)", false},

    // A URL with a user name or password in it falls through to the guesses.
    {"URL", R"(\A[A-Za-z][A-Za-z0-9+.-]*://(?![^/\s]*@)\S+\z)", false},
    {"URL", R"(\Awww\.[A-Za-z0-9-]+\.[A-Za-z]{2,}(?:[/?#]\S*)?\z)", true},
    {"URL", R"(\Amailto:[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}\z)", true},
    {"email address", R"(\A[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}\z)", false},
};

struct Compiled {
    const Shape* shape;
    QRegularExpression regex;
};

const std::vector<Compiled>& compiled()
{
    static const std::vector<Compiled> all = [] {
        std::vector<Compiled> v;
        for (const Shape& s : kShapes) {
            QRegularExpression::PatternOptions options = QRegularExpression::DotMatchesEverythingOption;
            if (s.caseless) {
                options |= QRegularExpression::CaseInsensitiveOption;
            }
            v.push_back({&s, QRegularExpression(QString::fromLatin1(s.pattern), options)});
        }
        return v;
    }();
    return all;
}

bool singleLine(const QString& text)
{
    return !text.isEmpty() && text.size() <= kMaxTextLength && !text.contains(QLatin1Char('\n'))
        && !text.contains(QLatin1Char('\r'));
}

// Glob match of `pattern` against `text` (both already case folded); only * is special.
bool globMatch(const QString& pattern, const QString& text)
{
    qsizetype p = 0;
    qsizetype t = 0;
    qsizetype star = -1;
    qsizetype mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern.at(p) == QLatin1Char('*')) {
            star = p++;
            mark = t;
        } else if (p < pattern.size() && pattern.at(p) == text.at(t)) {
            ++p;
            ++t;
        } else if (star >= 0) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern.at(p) == QLatin1Char('*')) {
        ++p;
    }
    return p == pattern.size();
}

} // namespace

QString normalize(const QString& rule)
{
    return rule.trimmed();
}

QString validate(const QString& ruleText)
{
    const QString rule = normalize(ruleText);
    if (rule.isEmpty()) {
        return QStringLiteral("the rule is empty");
    }
    if (rule.size() > kMaxRuleLength) {
        return QStringLiteral("the rule is longer than %1 characters").arg(kMaxRuleLength);
    }
    int stars = 0;
    for (const QChar c : rule) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            return QStringLiteral("the rule has a control character or line break");
        }
        if (c == QLatin1Char('*')) {
            ++stars;
        }
    }
    if (stars > kMaxWildcards) {
        return QStringLiteral("more than %1 wildcards (*)").arg(kMaxWildcards);
    }
    if (rule.size() - stars < kMinLiteral) {
        return QStringLiteral("needs at least %1 characters besides *").arg(kMinLiteral);
    }
    return {};
}

bool ruleMatches(const QString& rule, const QString& text)
{
    if (!validate(rule).isEmpty() || text.isEmpty() || text.size() > kMaxTextLength) {
        return false;
    }
    return globMatch(normalize(rule).toCaseFolded(), text.toCaseFolded());
}

int firstRuleMatch(const QStringList& rules, const QString& text)
{
    for (qsizetype i = 0; i < rules.size(); ++i) {
        if (ruleMatches(rules.at(i), text)) {
            return int(i);
        }
    }
    return -1;
}

QString builtinShape(const QString& text)
{
    if (!singleLine(text)) {
        return {};
    }
    for (const Compiled& c : compiled()) {
        if (c.regex.match(text).hasMatch()) {
            return QString::fromLatin1(c.shape->name);
        }
    }
    return {};
}

QStringList shapeNames()
{
    QStringList names;
    for (const Shape& s : kShapes) {
        const QString name = QString::fromLatin1(s.name);
        if (!names.contains(name)) {
            names << name;
        }
    }
    return names;
}

Verdict check(bool builtin, const QStringList& rules, const QString& text)
{
    Verdict v;
    if (!singleLine(text)) {
        return v;
    }
    const int rule = firstRuleMatch(rules, text);
    if (rule >= 0) {
        v.kind = Verdict::Kind::Rule;
        v.rule = rule;
        return v;
    }
    if (builtin) {
        v.shape = builtinShape(text);
        if (!v.shape.isEmpty()) {
            v.kind = Verdict::Kind::Builtin;
        }
    }
    return v;
}

QString describe(const Verdict& v)
{
    switch (v.kind) {
    case Verdict::Kind::Builtin:
        return QStringLiteral("built-in shape: %1").arg(v.shape);
    case Verdict::Kind::Rule:
        return QStringLiteral("your ignore rule %1").arg(v.rule + 1);
    case Verdict::Kind::None:
        break;
    }
    return {};
}

} // namespace clipitto::passwordignore
