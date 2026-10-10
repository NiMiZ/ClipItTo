#pragma once

#include <QString>

namespace clipitto {

// Points the GNOME custom keyboard shortcut "ClipItTo" at `command`, bound to
// `accelerator` (GTK syntax, e.g. "<Control>grave"), adding it to the custom-keybindings
// list if needed. Other custom shortcuts are left alone (same as scripts/clipitto-shortcut).
bool applyGnomeShortcut(const QString& accelerator, const QString& command, QString* error = nullptr,
                        const QString& gsettings = QStringLiteral("gsettings"));

} // namespace clipitto
