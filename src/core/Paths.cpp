#include "Paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace clipitto::paths {

QString configFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/clipitto/config.toml");
}

QString databaseFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/clipitto/history.db");
}

bool ensureParentDir(const QString& filePath)
{
    return QDir().mkpath(QFileInfo(filePath).absolutePath());
}

} // namespace clipitto::paths
