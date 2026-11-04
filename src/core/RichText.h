#pragma once

#include <QString>

namespace clipitto::richtext {

// Whether a copy's HTML says more than its plain text does: bold, italics, links, lists, tables, headings,
// pictures, and so on. Apps like browsers offer an HTML flavour for every copy; one that only wraps plain
// text (paragraphs, line breaks, a font or a colour) is not worth keeping. Looks at the markup only.
bool hasFormatting(const QString& html);

// Whether `html` is the formatted version of `text` and not of another copy (two clipboard reads can straddle a
// change): the first words of the text must all be found in the html's own text. Loose on purpose, as apps
// add bullets, numbers and tabs when they write the plain version.
bool matchesText(const QString& html, const QString& text);

} // namespace clipitto::richtext
