#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace clipitto {

// A user-defined rule for what is kept as a password copy (Settings > Passwords, `password_rules`).
// All counts are minimums: "digits 2" means at least two digits, and a copy of only two digits fits
// only if the length range allows it. Rules extend the built-in detection; they never remove from it.
struct PasswordRule {
    int minLength = 8;
    int maxLength = 64;
    int lower = 0;
    int upper = 0;
    int digits = 0;
    int symbols = 0; // anything that is not a letter, digit or white space
    bool digitsAtEnd = false; // the copy must end with a digit

    bool operator==(const PasswordRule&) const = default;
};

namespace passwordrules {

constexpr int kMaxRules = 5;
constexpr int kLengthLimit = 256;
// A rule must ask for at least this many characters, so a rule can never catch every short word.
constexpr int kMinLength = 6;

// Why a rule cannot be used, empty when it is fine. Checks: kMinLength <= min <= max <= kLengthLimit, counts
// 0..kLengthLimit, and the counts together fit in the maximum length.
QString validate(const PasswordRule& rule);

// Does the whole text (one line, no white space) fit the rule?
bool matches(const PasswordRule& rule, const QString& text);
// Does it fit any of the rules?
bool matchesAny(const QList<PasswordRule>& rules, const QString& text);
// Why the text does not fit the rule: a short reason class, never any part of the text. Empty when it fits.
QString whyNot(const PasswordRule& rule, const QString& text);

// Config text form: "8-20 lower=1 upper=1 digits=2 symbols=0 end". parse() returns false (and leaves
// `out` alone) for anything malformed or invalid.
QString format(const PasswordRule& rule);
bool parse(const QString& text, PasswordRule& out);

} // namespace passwordrules
} // namespace clipitto
