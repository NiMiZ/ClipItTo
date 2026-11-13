#pragma once

#include <QString>

namespace clipitto {

// Points the GNOME custom keyboard shortcut "ClipItTo" at `command`, bound to
// `accelerator` (GTK syntax, e.g. "<Control>grave"), adding it to the custom-keybindings
// list if needed. Other custom shortcuts are left alone (same as scripts/clipitto-shortcut).
// `slot` names the custom shortcut (its dconf path) and `name` what GNOME Settings shows: the default is the one that
// opens the popup; the panic key has its own.
bool applyGnomeShortcut(const QString& accelerator, const QString& command, QString* error = nullptr,
                        const QString& gsettings = QStringLiteral("gsettings"),
                        const QString& slot = QStringLiteral("clipitto"), const QString& name = QStringLiteral("ClipItTo"));

// Takes the custom shortcut `slot` out of the list and resets its values. Writes nothing when it is not there.
bool removeGnomeShortcut(QString* error = nullptr, const QString& gsettings = QStringLiteral("gsettings"),
                         const QString& slot = QStringLiteral("clipitto-panic"));

} // namespace clipitto
