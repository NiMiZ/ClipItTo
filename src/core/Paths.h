#pragma once

#include <QString>

// Where ClipItTo keeps its files.
//   Config:  Linux $XDG_CONFIG_HOME/clipitto (~/.config)     Windows %APPDATA%\clipitto (roams)
//   History: Linux $XDG_DATA_HOME/clipitto (~/.local/share)  Windows %LOCALAPPDATA%\clipitto
namespace clipitto::paths {

QString configDir();
QString configFile(); // <configDir>/config.toml
QString dataDir();
QString databaseFile(); // <dataDir>/history.db

// Creates the parent directory of a file path. Returns false on failure.
bool ensureParentDir(const QString& filePath);

} // namespace clipitto::paths
