#include "GnomeShortcut.h"

#include <QProcess>
#include <QStringList>

#include <utility>

namespace clipitto {

namespace {

const QString kSchema = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys");
const QString kPath = QStringLiteral("/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipitto/");

bool run(const QString& program, const QStringList& args, QString* output, QString* error)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForFinished(3000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
            *error = QStringLiteral("%1 %2 failed%3").arg(program, args.value(0),
                                                           detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
        return false;
    }
    if (output) {
        *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }
    return true;
}

// "@as []" / "['/a/', '/b/']" -> paths
QStringList parsePathList(QString value)
{
    if (value.startsWith(QLatin1String("@as "))) {
        value = value.mid(4);
    }
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('['))) {
        value = value.mid(1);
    }
    if (value.endsWith(QLatin1Char(']'))) {
        value.chop(1);
    }
    QStringList paths;
    for (QString item : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        item = item.trimmed();
        if (item.size() >= 2 && item.startsWith(QLatin1Char('\'')) && item.endsWith(QLatin1Char('\''))) {
            item = item.mid(1, item.size() - 2);
        }
        if (!item.isEmpty()) {
            paths << item;
        }
    }
    return paths;
}

} // namespace

bool applyGnomeShortcut(const QString& accelerator, const QString& command, QString* error, const QString& gsettings)
{
    QString current;
    if (!run(gsettings, {QStringLiteral("get"), kSchema, QStringLiteral("custom-keybindings")}, &current, error)) {
        return false;
    }
    QStringList paths = parsePathList(current);
    if (!paths.contains(kPath)) {
        paths << kPath;
        QStringList quoted;
        for (const QString& p : std::as_const(paths)) {
            quoted << QLatin1Char('\'') + p + QLatin1Char('\'');
        }
        const QString list = QLatin1Char('[') + quoted.join(QLatin1String(", ")) + QLatin1Char(']');
        if (!run(gsettings, {QStringLiteral("set"), kSchema, QStringLiteral("custom-keybindings"), list}, nullptr,
                 error)) {
            return false;
        }
    }
    const QString entry = kSchema + QStringLiteral(".custom-keybinding:") + kPath;
    return run(gsettings, {QStringLiteral("set"), entry, QStringLiteral("name"), QStringLiteral("ClipItTo")}, nullptr,
               error)
        && run(gsettings, {QStringLiteral("set"), entry, QStringLiteral("command"), command}, nullptr, error)
        && run(gsettings, {QStringLiteral("set"), entry, QStringLiteral("binding"), accelerator}, nullptr, error);
}

} // namespace clipitto
