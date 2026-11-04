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

// Does a whole copy look like a generated password (a browser's "new password" field, a generator)?
// One line, no spaces, 8-64 printable ASCII characters, at least three of lower case, upper case,
// digits and symbols, at least four letters, and not a URL, path, email address, colour, UUID, hash,
// date, call or a name made of plain words and numbers. A guess: it will take some ordinary copies,
// so callers keep it behind an off-by-default setting. Never logs the text.
bool looksLikeGeneratedPassword(const QString& text);

// Luhn check over the digits of `digits` (13-19 of them).
bool luhnValid(const QString& digits);

} // namespace secretscan
} // namespace clipitto
