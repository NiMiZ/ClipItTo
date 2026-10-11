#pragma once

#include "Config.h"

namespace clipitto {

// Types the paste shortcut into the focused window with synthetic key events:
// SendInput on Windows, the XTest extension on X11. Returns false where neither is
// available (Wayland, offscreen, builds without XTest).
bool pasteWithSyntheticKeys(PasteKeys keys);

// Remembers the window that has focus now, so that pasteWithSyntheticKeys() first brings it
// back: on Windows, hiding the popup activates whichever window is next in line, not
// necessarily the one the user came from. Elsewhere a no-op (X11 window managers and the
// GNOME extension return focus themselves). Call right before the popup takes focus.
void rememberPasteTarget();

} // namespace clipitto
