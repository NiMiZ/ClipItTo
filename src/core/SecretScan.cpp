#include "SecretScan.h"

#include <QRegularExpression>

#include <array>

namespace clipitto::secretscan {

namespace {

struct Pattern {
    const char* kind;
    QRegularExpression regex;
};

const std::array<Pattern, 11>& patterns()
{
    static const std::array<Pattern, 11> list = {{
        {"private key", QRegularExpression(QStringLiteral("-----BEGIN (?:[A-Z]+ )?PRIVATE KEY(?: BLOCK)?-----"))},
        {"AWS access key", QRegularExpression(QStringLiteral("\\b(?:AKIA|ASIA)[0-9A-Z]{16}\\b"))},
        {"GitHub token", QRegularExpression(QStringLiteral("\\bgh[pousr]_[A-Za-z0-9]{36,}\\b|\\bgithub_pat_[A-Za-z0-9_]{22,}\\b"))},
        {"GitLab token", QRegularExpression(QStringLiteral("\\bglpat-[A-Za-z0-9_-]{20,}\\b"))},
        {"Slack token", QRegularExpression(QStringLiteral("\\bxox[baprs]-[A-Za-z0-9-]{10,}\\b"))},
        {"Google API key", QRegularExpression(QStringLiteral("\\bAIza[0-9A-Za-z_-]{35}\\b"))},
        {"Stripe key", QRegularExpression(QStringLiteral("\\b[sr]k_(?:live|test)_[0-9A-Za-z]{16,}\\b"))},
        {"Anthropic key", QRegularExpression(QStringLiteral("\\bsk-ant-[A-Za-z0-9_-]{20,}"))},
        {"OpenAI key", QRegularExpression(QStringLiteral("\\bsk-(?:proj-)?[A-Za-z0-9_-]{32,}"))},
        {"JSON web token", QRegularExpression(QStringLiteral("\\beyJ[A-Za-z0-9_-]{8,}\\.eyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}"))},
        {"npm token", QRegularExpression(QStringLiteral("\\bnpm_[A-Za-z0-9]{36}\\b"))},
    }};
    return list;
}

// "4111 1111 1111 1111", "4111-1111-1111-1111" or 13-19 plain digits: the whole copy, nothing else.
const QRegularExpression& cardShape()
{
    static const QRegularExpression shape(QStringLiteral("^\\s*(\\d{4}[ -]?\\d{4}[ -]?\\d{4}[ -]?\\d{1,7}|\\d{13,19})\\s*$"));
    return shape;
}

} // namespace

bool luhnValid(const QString& digits)
{
    if (digits.size() < 13 || digits.size() > 19) {
        return false;
    }
    int sum = 0;
    bool second = false;
    for (qsizetype i = digits.size() - 1; i >= 0; --i) {
        const QChar c = digits.at(i);
        if (!c.isDigit()) {
            return false;
        }
        int d = c.digitValue();
        if (second) {
            d *= 2;
            if (d > 9) {
                d -= 9;
            }
        }
        sum += d;
        second = !second;
    }
    return sum % 10 == 0;
}

QString whyNotGeneratedPassword(const QString& text)
{
    if (text.size() < 8 || text.size() > 64) {
        return QStringLiteral("length outside 8 to 64");
    }
    bool lower = false, upper = false, digit = false, symbol = false;
    int letters = 0;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u <= 0x20 || u >= 0x7f) {
            // spaces, line breaks, control and non-ASCII characters: not a generated password
            return QStringLiteral("has spaces, line breaks or non-ASCII characters");
        }
        if (u >= 'a' && u <= 'z') {
            lower = true;
            ++letters;
        } else if (u >= 'A' && u <= 'Z') {
            upper = true;
            ++letters;
        } else if (u >= '0' && u <= '9') {
            digit = true;
        } else {
            symbol = true;
        }
    }
    if (int(lower) + int(upper) + int(digit) + int(symbol) < 3) {
        return QStringLiteral("too few character classes (needs 3 of lower, upper, digit, symbol)");
    }
    if (letters < 4) {
        return QStringLiteral("fewer than 4 letters");
    }
    static const QRegularExpression notPassword(QStringLiteral(
        "^(?:[A-Za-z][A-Za-z0-9+.-]*://.*"                                                   // URL
        "|www\\..*"
        "|[A-Za-z]:[\\\\/].*"                                                                // C:\\dir
        "|[/~\\\\].*"                                                                        // /path, ~/path, \\server
        "|[^@]+@[^@]+\\.[A-Za-z]{2,}"                                                        // email address
        "|#[0-9A-Fa-f]{3,8}"                                                                 // colour
        "|(?:rgb|hsl)a?\\(.*"
        "|[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"       // UUID
        "|[0-9A-Fa-f]{32,}"                                                                  // hash
        "|\\d{4}-\\d{2}-\\d{2}.*"                                                            // date
        "|.*\\(\\).*)$"));                                                                   // call: foo()
    if (text.contains(notPassword)) {
        return QStringLiteral("looks like a URL, path, email, colour, UUID, hash, date or call");
    }
    // Plain words and numbers joined by _ - . / \ : (camelCase names, snake_case, file names,
    // versions, "Password123") are text: a generated password has digits or symbols inside its words.
    static const QRegularExpression wordLike(QStringLiteral("^[A-Za-z]*\\d{0,4}$"));
    static const QRegularExpression joiners(QStringLiteral("[_./\\\\:-]"));
    for (const QString& part : text.split(joiners)) {
        if (!part.contains(wordLike)) {
            return QString();
        }
    }
    return QStringLiteral("looks like words and numbers joined together");
}

bool looksLikeGeneratedPassword(const QString& text)
{
    return whyNotGeneratedPassword(text).isEmpty();
}

QString kindOf(const QString& text)
{
    constexpr qsizetype kLimit = 64 * 1024;
    const QString head = text.size() > kLimit ? text.left(kLimit) : text;
    for (const Pattern& p : patterns()) {
        if (head.contains(p.regex)) {
            return QString::fromLatin1(p.kind);
        }
    }
    if (head.size() <= 40 && head.contains(cardShape())) {
        QString digits;
        for (const QChar c : head) {
            if (c.isDigit()) {
                digits.append(c);
            }
        }
        if (luhnValid(digits)) {
            return QStringLiteral("card number");
        }
    }
    return {};
}

} // namespace clipitto::secretscan
