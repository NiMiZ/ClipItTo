#pragma once

#include <QList>
#include <QString>

#include <cstdint>
#include <optional>

namespace clipitto {

// Recognises a copy that is a single whole number (decimal or hexadecimal, up to 64 bits) and says which
// bases it can be read in. Shared by the "Number" items of Paste as (bit numbers, and later base conversion).
namespace numbertext {

struct Number {
    // "0x2c" and "2c" can only be hex; "123" can be read either way.
    bool canBeHex = false;
    bool canBeDec = false;
    std::uint64_t hexValue = 0; // valid when canBeHex
    std::uint64_t decValue = 0; // valid when canBeDec
    bool hasPrefix = false;     // written with 0x
};

// The number in `text` (surrounding whitespace ignored), or nothing when the text is not exactly one number,
// is negative, or does not fit in 64 bits.
std::optional<Number> parse(const QString& text);

// Numbers of the set bits of `value`, 0-based from the least significant bit, highest first: 0x123 -> 8 5 1 0.
QList<int> setBits(std::uint64_t value);
// Those numbers separated by spaces ("8 5 1 0").
QString bitsText(std::uint64_t value);

// `value` as lowercase hex without a prefix ("2c"), as decimal ("44"), and as binary padded to whole bytes
// ("00101100"; 0 is "00000000").
QString hexText(std::uint64_t value);
QString decText(std::uint64_t value);
QString binaryText(std::uint64_t value);

} // namespace numbertext
} // namespace clipitto
