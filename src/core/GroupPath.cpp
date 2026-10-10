#include "GroupPath.h"

#include <QRegularExpression>
#include <QStringList>

namespace clipitto::groups {

namespace {

const QString kDisplaySeparator = QStringLiteral(" › ");

} // namespace

QString normalize(const QString& path)
{
    static const QRegularExpression separators(QStringLiteral("[/›]"));
    QStringList levels;
    for (const QString& level : path.split(separators)) {
        const QString trimmed = level.simplified();
        if (!trimmed.isEmpty()) {
            levels << trimmed;
        }
    }
    return levels.join(kSeparator);
}

QString parent(const QString& path)
{
    const qsizetype cut = path.lastIndexOf(kSeparator);
    return cut < 0 ? QString() : path.left(cut);
}

QString leaf(const QString& path)
{
    return path.mid(path.lastIndexOf(kSeparator) + 1);
}

int depth(const QString& path)
{
    return int(path.count(kSeparator));
}

bool isWithin(const QString& path, const QString& ancestor)
{
    return path == ancestor || (path.startsWith(ancestor) && path.size() > ancestor.size()
                                && path.at(ancestor.size()) == kSeparator);
}

QString display(const QString& path)
{
    return QString(path).replace(kSeparator, kDisplaySeparator);
}

} // namespace clipitto::groups
