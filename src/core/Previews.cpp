#include "Previews.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace clipitto::preview {

namespace {

constexpr int kMaxFiles = 200;

std::optional<int> hexPair(const QString& s, int pos, int digits)
{
    bool ok = false;
    const int v = s.mid(pos, digits).toInt(&ok, 16);
    if (!ok) {
        return std::nullopt;
    }
    return digits == 1 ? v * 17 : v;
}

// 0..255 from "12", or "50%".
std::optional<int> channel(QString part)
{
    part = part.trimmed();
    const bool percent = part.endsWith(QLatin1Char('%'));
    if (percent) {
        part.chop(1);
    }
    bool ok = false;
    const double v = part.toDouble(&ok);
    if (!ok || v < 0) {
        return std::nullopt;
    }
    const double scaled = percent ? v * 255 / 100 : v;
    return scaled > 255.5 ? std::nullopt : std::optional<int>(qRound(scaled));
}

std::optional<int> alphaOf(QString part)
{
    part = part.trimmed();
    const bool percent = part.endsWith(QLatin1Char('%'));
    if (percent) {
        part.chop(1);
    }
    bool ok = false;
    const double v = part.toDouble(&ok);
    const double unit = percent ? v / 100 : v;
    return ok && unit >= 0 && unit <= 1 ? std::optional<int>(qRound(unit * 255)) : std::nullopt;
}

bool isPath(const QString& line)
{
    static const QRegularExpression unix(QStringLiteral(R"(^(~|)/[^/\0]+(/[^\0]*)+$)")); // /a/b, ~/a (not /a)
    static const QRegularExpression home(QStringLiteral(R"(^~/[^\0]+$)"));
    static const QRegularExpression drive(QStringLiteral(R"(^[A-Za-z]:[\\/][^\0<>|"?*]+$)"));
    static const QRegularExpression unc(QStringLiteral(R"(^\\\\[^\\\0]+\\[^\0<>|"?*]+$)"));
    return unix.match(line).hasMatch() || home.match(line).hasMatch() || drive.match(line).hasMatch()
        || unc.match(line).hasMatch();
}

QString fileName(const QString& path)
{
    const int cut = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    const QString name = path.mid(cut + 1);
    return name.isEmpty() ? path : name;
}

} // namespace

std::optional<Rgba> colorOf(const QString& text)
{
    if (text.size() > 80) {
        return std::nullopt; // before trimming: this runs for every painted row
    }
    const QString t = text.trimmed();
    if (t.isEmpty() || t.size() > 40 || t.contains(QLatin1Char('\n'))) {
        return std::nullopt;
    }
    if (t.startsWith(QLatin1Char('#'))) {
        static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]+$"));
        if (!hex.match(t).hasMatch()) {
            return std::nullopt;
        }
        const int digits = int(t.size()) - 1;
        if (digits != 3 && digits != 4 && digits != 6 && digits != 8) {
            return std::nullopt;
        }
        const int step = digits <= 4 ? 1 : 2;
        const auto r = hexPair(t, 1, step), g = hexPair(t, 1 + step, step), b = hexPair(t, 1 + 2 * step, step);
        const auto a = digits == 4 || digits == 8 ? hexPair(t, 1 + 3 * step, step) : std::optional<int>(255);
        return Rgba{*r, *g, *b, *a};
    }
    static const QRegularExpression fn(QStringLiteral(R"(^rgba?\(\s*([^,\s]+)\s*[, ]\s*([^,\s]+)\s*[, ]\s*([^,\s/]+)\s*(?:[,/]\s*([^,\s)]+)\s*)?\)$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = fn.match(t);
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    const auto r = channel(m.captured(1)), g = channel(m.captured(2)), b = channel(m.captured(3));
    if (!r || !g || !b) {
        return std::nullopt;
    }
    int a = 255;
    if (!m.captured(4).isEmpty()) {
        const auto parsed = alphaOf(m.captured(4));
        if (!parsed) {
            return std::nullopt;
        }
        a = *parsed;
    }
    return Rgba{*r, *g, *b, a};
}

QStringList filePaths(const QString& text)
{
    if (text.size() > 64 * 1024) {
        return {};
    }
    QStringList paths;
    for (const QString& raw : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
            const QUrl url(line, QUrl::StrictMode);
            const bool here = url.host().isEmpty() || url.host() == QLatin1String("localhost");
            const QString local = url.isLocalFile() && here ? url.toLocalFile() : QString();
            if (local.isEmpty()) {
                return {};
            }
            paths << local;
        } else if (isPath(line)) {
            paths << line;
        } else {
            return {};
        }
        if (paths.size() > kMaxFiles) {
            return {};
        }
    }
    return paths;
}

QString copiedFilesText(const QString& raw)
{
    constexpr int kMaxPathLength = 4096;
    if (raw.size() > 64 * 1024) {
        return {};
    }
    QStringList paths;
    bool first = true;
    for (const QString& part : raw.split(QLatin1Char('\n'))) {
        const QString line = part.trimmed(); // also drops the CR of a CRLF list
        if (first) {
            first = false;
            // Nautilus: the action comes first.
            if (line == QLatin1String("copy") || line == QLatin1String("cut")) {
                continue;
            }
        }
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (!line.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
            return {};
        }
        QUrl url(line, QUrl::StrictMode);
        const bool here = url.host().isEmpty() || url.host() == QLatin1String("localhost");
        if (!url.isValid() || !url.isLocalFile() || !here || url.hasQuery() || url.hasFragment()) {
            return {};
        }
        url.setHost(QString()); // file://localhost/x is /x, not a share called localhost
        const QString path = url.toLocalFile();
        if (path.isEmpty() || path.size() > kMaxPathLength || !path.startsWith(QLatin1Char('/'))) {
            return {};
        }
        for (const QChar c : path) {
            if (c.unicode() < 0x20 || c.unicode() == 0x7f || c == QChar(0xFFFD)) {
                return {}; // a newline would forge a line; U+FFFD means invalid UTF-8
            }
        }
        for (const QString& name : path.split(QLatin1Char('/'))) {
            if (name == QLatin1String("..") || name == QLatin1String(".")) {
                return {};
            }
        }
        paths << path;
        if (paths.size() > kMaxFiles) {
            return {};
        }
    }
    return paths.join(QLatin1Char('\n'));
}

namespace {
const QString kFilesPrefix = QStringLiteral("x-clipitto-files:");
}

QString filesMimeType(const QString& format)
{
    return kFilesPrefix + format;
}

bool hasFilesMimeType(const QStringList& mimeTypes)
{
    return std::any_of(mimeTypes.begin(), mimeTypes.end(),
                       [](const QString& type) { return type.startsWith(kFilesPrefix); });
}

QStringList withoutFilesMimeType(QStringList mimeTypes)
{
    mimeTypes.removeIf([](const QString& type) { return type.startsWith(kFilesPrefix); });
    return mimeTypes;
}

QString fileSummary(const QStringList& paths)
{
    if (paths.isEmpty()) {
        return {};
    }
    if (paths.size() == 1) {
        return paths.first(); // a lone path may be just text from a chat or terminal: show all of it
    }
    QStringList names;
    for (int i = 0; i < paths.size() && i < 2; ++i) {
        names << fileName(paths.at(i));
    }
    QString summary = names.join(QStringLiteral(", "));
    if (paths.size() > 2) {
        summary += QCoreApplication::translate("clipitto::preview", ", +%n more", nullptr, int(paths.size()) - 2);
    }
    return summary;
}

} // namespace clipitto::preview
