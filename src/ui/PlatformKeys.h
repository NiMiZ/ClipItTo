#pragma once

#include <QString>

namespace clipitto::ui {

// Key names in on-screen help as the platform labels them. In Qt the Control modifier is the
// ⌘ key on a Mac (and Alt is ⌥), so the popup's Ctrl+S is ⌘S there: help text written with
// "Ctrl+" and "Alt+" is shown with the Mac symbols.
inline QString platformKeys(QString text)
{
#ifdef Q_OS_MACOS
    text.replace(QStringLiteral("Ctrl+"), QStringLiteral("⌘"));
    text.replace(QStringLiteral("Alt+"), QStringLiteral("⌥"));
#endif
    return text;
}

} // namespace clipitto::ui
