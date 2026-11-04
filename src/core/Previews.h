#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace clipitto::preview {

struct Rgba {
    int r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Rgba&) const = default;
};

// The text is one colour and nothing else: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb(r, g, b)` or
// `rgba(r, g, b, a)` (a: 0 to 1, or a percent), for the swatch in the list.
std::optional<Rgba> colorOf(const QString& text);

// The text is a list of files: every non-empty line is an absolute path (`/home/me/a.txt`, `~/a.txt`, `C:\a.txt`,
// `\\server\share\a.txt`) or a `file://` URI. Returns the paths (URIs decoded); empty when any line is something
// else, or there are too many. Looks at the text only, never at the disk. A lone `/help` is not a path.
QStringList filePaths(const QString& text);

// "a.txt", or "a.txt, b.txt, +2 more" for the row's title.
QString fileSummary(const QStringList& paths);

} // namespace clipitto::preview
