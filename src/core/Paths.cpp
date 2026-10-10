#include "Paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace clipitto::paths {

namespace {

const QString kAppDir = QStringLiteral("clipitto");

} // namespace

QString configDir()
{
#ifdef Q_OS_WIN
    // Settings follow the user's roaming profile. Qt has no generic roaming location
    // (GenericConfigLocation is %LOCALAPPDATA% on Windows), so ask the environment.
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    }
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#endif
    return QDir(base).filePath(kAppDir);
}

QString configFile()
{
    return QDir(configDir()).filePath(QStringLiteral("config.toml"));
}

QString dataDir()
{
    // Machine-local on both platforms: $XDG_DATA_HOME, %LOCALAPPDATA%.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(kAppDir);
}

QString databaseFile()
{
    return QDir(dataDir()).filePath(QStringLiteral("history.db"));
}

bool ensureParentDir(const QString& filePath)
{
    return QDir().mkpath(QFileInfo(filePath).absolutePath());
}

} // namespace clipitto::paths
