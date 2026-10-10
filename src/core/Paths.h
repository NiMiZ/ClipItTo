#pragma once

#include <QString>

// XDG locations used by ClipItTo.
namespace clipitto::paths {

QString configFile(); // ~/.config/clipitto/config.toml
QString databaseFile(); // ~/.local/share/clipitto/history.db

// Creates the parent directory of a file path. Returns false on failure.
bool ensureParentDir(const QString& filePath);

} // namespace clipitto::paths
