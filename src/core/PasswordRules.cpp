#include "PasswordRules.h"

namespace clipitto::passwordrules {

namespace {

struct Counts {
    int lower = 0;
    int upper = 0;
    int digits = 0;
    int symbols = 0;
};

Counts countOf(const QString& text)
{
    Counts c;
    for (const QChar ch : text) {
        if (ch.isLower()) {
            ++c.lower;
        } else if (ch.isUpper()) {
            ++c.upper;
        } else if (ch.isDigit()) {
            ++c.digits;
        } else if (!ch.isLetter() && !ch.isSpace()) {
            ++c.symbols;
        }
    }
    return c;
}

} // namespace

QString validate(const PasswordRule& r)
{
    if (r.minLength < kMinLength || r.maxLength > kLengthLimit) {
        return QStringLiteral("length must be between %1 and %2").arg(kMinLength).arg(kLengthLimit);
    }
    if (r.minLength > r.maxLength) {
        return QStringLiteral("the shortest length is above the longest");
    }
    for (const int n : {r.lower, r.upper, r.digits, r.symbols}) {
        if (n < 0 || n > kLengthLimit) {
            return QStringLiteral("a minimum count is out of range");
        }
    }
    if (r.lower + r.upper + r.digits + r.symbols > r.maxLength) {
        return QStringLiteral("the minimum counts together are more than the longest length");
    }
    return {};
}

QString whyNot(const PasswordRule& r, const QString& text)
{
    if (text.isEmpty() || text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'))) {
        return QStringLiteral("not a single line");
    }
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            return QStringLiteral("contains white space");
        }
    }
    if (text.size() < r.minLength) {
        return QStringLiteral("too short");
    }
    if (text.size() > r.maxLength) {
        return QStringLiteral("too long");
    }
    const Counts c = countOf(text);
    if (c.lower < r.lower) {
        return QStringLiteral("too few lower case letters");
    }
    if (c.upper < r.upper) {
        return QStringLiteral("too few upper case letters");
    }
    if (c.digits < r.digits) {
        return QStringLiteral("too few digits");
    }
    if (c.symbols < r.symbols) {
        return QStringLiteral("too few symbols");
    }
    if (r.digitsAtEnd && !text.at(text.size() - 1).isDigit()) {
        return QStringLiteral("does not end with a digit");
    }
    return {};
}

bool matches(const PasswordRule& rule, const QString& text)
{
    return whyNot(rule, text).isEmpty();
}

bool matchesAny(const QList<PasswordRule>& rules, const QString& text)
{
    for (const PasswordRule& r : rules) {
        if (matches(r, text)) {
            return true;
        }
    }
    return false;
}

QString format(const PasswordRule& r)
{
    QString s = QStringLiteral("%1-%2 lower=%3 upper=%4 digits=%5 symbols=%6")
                    .arg(r.minLength).arg(r.maxLength).arg(r.lower).arg(r.upper).arg(r.digits).arg(r.symbols);
    if (r.digitsAtEnd) {
        s += QStringLiteral(" end");
    }
    return s;
}

bool parse(const QString& text, PasswordRule& out)
{
    const QStringList parts = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }
    PasswordRule r;
    const QStringList range = parts.first().split(QLatin1Char('-'));
    if (range.size() != 2) {
        return false;
    }
    bool ok1 = false;
    bool ok2 = false;
    r.minLength = range.at(0).toInt(&ok1);
    r.maxLength = range.at(1).toInt(&ok2);
    if (!ok1 || !ok2) {
        return false;
    }
    for (qsizetype i = 1; i < parts.size(); ++i) {
        const QString& p = parts.at(i);
        if (p == QLatin1String("end")) {
            r.digitsAtEnd = true;
            continue;
        }
        const qsizetype eq = p.indexOf(QLatin1Char('='));
        if (eq < 0) {
            return false;
        }
        const QString key = p.left(eq);
        bool ok = false;
        const int n = p.mid(eq + 1).toInt(&ok);
        if (!ok) {
            return false;
        }
        if (key == QLatin1String("lower")) {
            r.lower = n;
        } else if (key == QLatin1String("upper")) {
            r.upper = n;
        } else if (key == QLatin1String("digits")) {
            r.digits = n;
        } else if (key == QLatin1String("symbols")) {
            r.symbols = n;
        } else {
            return false;
        }
    }
    if (!validate(r).isEmpty()) {
        return false;
    }
    out = r;
    return true;
}

} // namespace clipitto::passwordrules
