#pragma once

#include "Config.h"

namespace clipitto {

// Types the paste shortcut into the focused window with synthetic key events:
// SendInput on Windows, the XTest extension on X11, ⌘V through Quartz events on macOS (which
// needs the Accessibility permission). Returns false where none is available (Wayland,
// offscreen, builds without XTest, no permission).
bool pasteWithSyntheticKeys(PasteKeys keys);

// Remembers the window that has focus now, so that pasteWithSyntheticKeys() first brings it
// back: on Windows, hiding the popup activates whichever window is next in line, not
// necessarily the one the user came from. Elsewhere a no-op (X11 window managers and the
// GNOME extension return focus themselves). Call right before the popup takes focus.
void rememberPasteTarget();

// What rememberPasteTarget() saw, as names that identify the program (never a window title); see
// PasteKeys.h for the three answers. The question is what receives the paste, so each platform says
// what it can really tell:
//   Windows: the foreground window's executable name and window class
//   X11:     _NET_ACTIVE_WINDOW's WM_CLASS (instance and class); a Wayland session: unsupported
//   macOS:   unsupported (⌘V everywhere; there is nothing to choose)
//   elsewhere, builds without XTest: unsupported
PasteTarget rememberedPasteTarget();

} // namespace clipitto
