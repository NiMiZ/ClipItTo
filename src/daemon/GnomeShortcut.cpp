#include "GnomeShortcut.h"

#include <QProcess>
#include <QStringList>

#include <utility>

namespace clipitto {

namespace {

const QString kSchema = QStringLiteral("org.gnome.settings-daemon.plugins.media-keys");
const QString kPathBase = QStringLiteral("/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/");

QString pathFor(const QString& slot)
{
    return kPathBase + slot + QLatin1Char('/');
}

QString listValue(const QStringList& paths)
{
    QStringList quoted;
    for (const QString& p : paths) {
        quoted << QLatin1Char('\'') + p + QLatin1Char('\'');
    }
    return QLatin1Char('[') + quoted.join(QLatin1String(", ")) + QLatin1Char(']');
}

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

// Writes `value` only when the key holds something else, so a normal start is read-only.
// gsettings prints strings in single quotes ('ClipItTo').
bool setIfChanged(const QString& gsettings, const QString& entry, const QString& key, const QString& value,
                  QString* error)
{
    QString current;
    if (run(gsettings, {QStringLiteral("get"), entry, key}, &current, nullptr)) {
        if (current.size() >= 2 && current.startsWith(QLatin1Char('\'')) && current.endsWith(QLatin1Char('\''))) {
            current = current.mid(1, current.size() - 2);
        }
        if (current == value) {
            return true;
        }
    }
    return run(gsettings, {QStringLiteral("set"), entry, key, value}, nullptr, error);
}

} // namespace

bool applyGnomeShortcut(const QString& accelerator, const QString& command, QString* error, const QString& gsettings,
                        const QString& slot, const QString& name)
{
    const QString kPath = pathFor(slot);
    QString current;
    if (!run(gsettings, {QStringLiteral("get"), kSchema, QStringLiteral("custom-keybindings")}, &current, error)) {
        return false;
    }
    QStringList paths = parsePathList(current);
    if (!paths.contains(kPath)) {
        paths << kPath;
        const QString list = listValue(paths);
        if (!run(gsettings, {QStringLiteral("set"), kSchema, QStringLiteral("custom-keybindings"), list}, nullptr,
                 error)) {
            return false;
        }
    }
    const QString entry = kSchema + QStringLiteral(".custom-keybinding:") + kPath;
    return setIfChanged(gsettings, entry, QStringLiteral("name"), name, error)
        && setIfChanged(gsettings, entry, QStringLiteral("command"), command, error)
        && setIfChanged(gsettings, entry, QStringLiteral("binding"), accelerator, error);
}

bool removeGnomeShortcut(QString* error, const QString& gsettings, const QString& slot)
{
    const QString path = pathFor(slot);
    QString current;
    if (!run(gsettings, {QStringLiteral("get"), kSchema, QStringLiteral("custom-keybindings")}, &current, error)) {
        return false;
    }
    QStringList paths = parsePathList(current);
    if (!paths.removeAll(path)) {
        return true; // not there: nothing written
    }
    // Out of the list first (the shortcut stops working), then its values are reset.
    if (!run(gsettings, {QStringLiteral("set"), kSchema, QStringLiteral("custom-keybindings"), listValue(paths)}, nullptr,
             error)) {
        return false;
    }
    const QString entry = kSchema + QStringLiteral(".custom-keybinding:") + path;
    for (const char* key : {"binding", "command", "name"}) {
        run(gsettings, {QStringLiteral("reset"), entry, QString::fromLatin1(key)}, nullptr, nullptr); // best effort
    }
    return true;
}

} // namespace clipitto
