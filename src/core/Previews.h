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

// Files copied in a file manager on GNOME, as the shell extension hands them over: `raw` is the content of the clipboard
// format `x-special/gnome-copied-files` ("copy" or "cut", then one `file://` URI per line) or `text/uri-list` (one URI per
// line, `#` comments). The paths come back decoded, one per line, ready to be the text of a file-list copy. Only `file:`
// URIs of this computer (no host, or `localhost`) are accepted; percent escapes are decoded as UTF-8. A path with a control
// character (a `%0A` would forge a second line), an invalid UTF-8 escape, a `..` or `.` part, or more than 4096 characters
// refuses the whole list, and so does more than 200 files, more than 64 KiB of input, or any line that is not a `file:` URI
// (a `smb://` or `https://` entry). Returns an empty string when refused or empty. Looks at the text only.
QString copiedFilesText(const QString& raw);

// The pseudo MIME type the shell extension adds to say the text of an `Ingest` call is a file list in one of the formats above
// ("x-clipitto-files:gnome" or "x-clipitto-files:uri-list"), the same way "x-clipitto-source:" names the source app.
// An app can't name it itself: the extension drops any it finds in what the clipboard owner offers.
QString filesMimeType(const QString& format);
bool hasFilesMimeType(const QStringList& mimeTypes);
QStringList withoutFilesMimeType(QStringList mimeTypes);

// The row's title: the whole path for one file, "a.txt, b.txt, +2 more" (names only) for several.
QString fileSummary(const QStringList& paths);

} // namespace clipitto::preview
