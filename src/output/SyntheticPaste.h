#pragma once

#include "Config.h"

namespace clipitto {

// Types the paste shortcut into the focused window with synthetic key events:
// SendInput on Windows, the XTest extension on X11. Returns false where neither is
// available (Wayland, offscreen, builds without XTest).
bool pasteWithSyntheticKeys(PasteKeys keys);

} // namespace clipitto
