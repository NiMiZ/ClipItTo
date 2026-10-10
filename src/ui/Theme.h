#pragma once

#include "Config.h"

#include <QPalette>

namespace clipitto::ui {

// Applies the theme to the whole application. Light and Dark use the Fusion style with a
// matching palette; System keeps the platform look, switching to Fusion dark when the
// desktop prefers dark (and follows desktop changes where Qt reports them).
void applyTheme(Theme theme);

// The desktop's light/dark preference: Qt (6.5+), else GNOME's color-scheme setting or the
// Windows "apps use light theme" setting, else the lightness of the default palette.
bool systemPrefersDark();

QPalette darkPalette();

} // namespace clipitto::ui
