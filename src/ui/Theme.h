#pragma once

#include "Config.h"

#include <QPalette>

#include <functional>

namespace clipitto::ui {

// Applies the theme to the whole application. Light and Dark use the Fusion style with a
// matching palette; System keeps the platform look, switching to Fusion dark when the
// desktop prefers dark (and follows desktop changes where Qt reports them).
void applyTheme(Theme theme);

// The desktop's light/dark preference: Qt (6.5+), else GNOME's color-scheme setting or the
// Windows "apps use light theme" setting, else the lightness of the default palette.
bool systemPrefersDark();

QPalette darkPalette();
// Fixed light colors: Fusion's own palette follows the desktop, so it can be dark.
QPalette lightPalette();

// Whether the applied theme is dark (for System: what the desktop prefers right now).
bool currentThemeIsDark();

// Calls `callback` after every theme change: applyTheme(), and the desktop switching between
// light and dark while the theme is System (Qt 6.5+). For things outside the palette, like
// the tray icon.
void onThemeApplied(std::function<void()> callback);

} // namespace clipitto::ui
