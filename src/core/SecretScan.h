#pragma once

#include <QString>

namespace clipitto {

// Does a copy look like a secret that was not marked as one by a password manager: a private key,
// an API key or access token of a well-known service, or a card number? Used to keep such copies
// as password copies (encrypted) instead of plain text. Never logs the text.
namespace secretscan {

// Name of the first kind found ("private key", "AWS access key", "card number", ...), empty when
// nothing looks secret. Only the first 64 KiB are looked at.
QString kindOf(const QString& text);
inline bool looksSecret(const QString& text) { return !kindOf(text).isEmpty(); }

// Luhn check over the digits of `digits` (13-19 of them).
bool luhnValid(const QString& digits);

} // namespace secretscan
} // namespace clipitto
