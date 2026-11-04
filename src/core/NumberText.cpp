#include "NumberText.h"

#include <QStringList>

namespace clipitto::numbertext {

namespace {

// `digits` in `base` (10 or 16) into `out`; false on a bad digit or a value above 64 bits.
bool convert(const QString& digits, int base, std::uint64_t& out)
{
    if (digits.isEmpty()) {
        return false;
    }
    const auto b = static_cast<std::uint64_t>(base);
    std::uint64_t value = 0;
    for (const QChar ch : digits) {
        const char16_t c = ch.unicode();
        int d = -1;
        if (c >= u'0' && c <= u'9') {
            d = c - u'0';
        } else if (c >= u'a' && c <= u'f') {
            d = c - u'a' + 10;
        } else if (c >= u'A' && c <= u'F') {
            d = c - u'A' + 10;
        }
        if (d < 0 || d >= base) {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(d);
        if (value > (UINT64_MAX - digit) / b) {
            return false; // would not fit in 64 bits
        }
        value = value * b + digit;
    }
    out = value;
    return true;
}

} // namespace

std::optional<Number> parse(const QString& text)
{
    const QString s = text.trimmed();
    if (s.isEmpty()) {
        return std::nullopt;
    }
    Number n;
    if (s.size() > 2 && s[0] == QLatin1Char('0') && (s[1] == QLatin1Char('x') || s[1] == QLatin1Char('X'))) {
        n.hasPrefix = true;
        n.canBeHex = convert(s.mid(2), 16, n.hexValue);
        return n.canBeHex ? std::optional<Number>(n) : std::nullopt;
    }
    // Only digits: both readings (a reading that overflows 64 bits is dropped). With a letter: hex only.
    n.canBeDec = convert(s, 10, n.decValue);
    n.canBeHex = convert(s, 16, n.hexValue);
    if (n.canBeDec || n.canBeHex) {
        return n;
    }
    return std::nullopt;
}

QList<int> setBits(std::uint64_t value)
{
    QList<int> bits;
    for (int i = 63; i >= 0; --i) {
        if ((value >> i) & 1U) {
            bits.append(i);
        }
    }
    return bits;
}

QString bitsText(std::uint64_t value)
{
    QStringList parts;
    for (const int b : setBits(value)) {
        parts << QString::number(b);
    }
    return parts.join(QLatin1Char(' '));
}

} // namespace clipitto::numbertext
