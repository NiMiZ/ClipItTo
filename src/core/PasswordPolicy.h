#pragma once

#include <QList>
#include <QString>

namespace clipitto {

// What a new master password has to be. Each rule is shown to the user while typing.
namespace passwordpolicy {

constexpr int kMinLength = 8;

struct Rule {
    QString text; // "At least 8 characters"
    bool met = false;
};

// All rules, in display order, for `password`. `userName` is the OS account name (rules out
// using it; ignored when shorter than 3 characters).
QList<Rule> check(const QString& password, const QString& userName);
// Every rule met.
bool acceptable(const QString& password, const QString& userName);
// The OS account name, for check().
QString currentUserName();

// Lower case, look-alike symbols turned into letters (@ -> a, 0 -> o, $ -> s, ...) and
// everything but letters and digits dropped: "P@ss-W0rd" -> "password". For matching only.
QString normalized(const QString& text);

} // namespace passwordpolicy
} // namespace clipitto
