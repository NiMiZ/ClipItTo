#pragma once

#include <QIcon>

namespace clipitto::ui {

// ClipItTo's icon (a clipboard), painted at the usual sizes; used for the tray and windows.
// The dark variant (light clip and lines on a dark sheet) suits dark themes.
QIcon appIcon(bool dark = false);

} // namespace clipitto::ui
