#pragma once

#include <QString>
#include <QStringList>

namespace clipitto {

// Copies that are clearly not passwords although they have the shape of one: an environment variable
// (%AppData%\rig, $HOME/bin), a file path, a URL, an email address... Two layers:
//   * built-in shapes (config `password_ignore_builtin`, on by default), and
//   * your own rules (`password_ignore`, up to 16): a text with * wildcards, matched against the whole copy.
// They are used only for what ClipItTo GUESSES is a password (detect_passwords and password_rules). Copies a
// password manager marked, and keys, tokens and card numbers (secretscan::kindOf), are never ignored.
// Pure functions; none of them logs or returns any part of the text.
namespace passwordignore {

constexpr int kMaxRules = 16;
constexpr int kMaxWildcards = 3;
constexpr int kMinLiteral = 3;       // characters other than * a rule must have, so a rule cannot ignore everything
constexpr int kMaxRuleLength = 200;
constexpr int kMaxTextLength = 1024; // longer texts are not looked at (the guesses never take them)

// Why a rule cannot be used, empty when it is fine.
QString validate(const QString& rule);
// The rule as stored: surrounding white space removed.
QString normalize(const QString& rule);
// Does the whole text fit the rule (* = any characters, letter case ignored)? False for an invalid rule.
bool ruleMatches(const QString& rule, const QString& text);
// Index of the first valid rule the text fits, -1 for none.
int firstRuleMatch(const QStringList& rules, const QString& text);

// Name of the built-in shape the text has ("environment variable", "file path", "URL", ...), empty for none.
QString builtinShape(const QString& text);
// The names of all shapes, in the order of the table, each once (for the tooltip and the docs).
QStringList shapeNames();

struct Verdict {
    enum class Kind { None, Builtin, Rule };
    Kind kind = Kind::None;
    QString shape; // Builtin: the shape's name
    int rule = -1; // Rule: index into the rules
    bool ignored() const { return kind != Kind::None; }
};
// Rules first, then the shapes (when `builtin`). Multi-line and very long texts are never ignored here: the guesses
// do not take them either.
Verdict check(bool builtin, const QStringList& rules, const QString& text);
// A short class of reason ("built-in shape: file path", "your ignore rule 2"), never any part of the text.
QString describe(const Verdict& verdict);

} // namespace passwordignore
} // namespace clipitto
