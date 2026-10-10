#pragma once

#include "Config.h"

namespace clipitto {

// Types the paste shortcut into the focused window with the XTest extension.
// Returns false when not running on X11 or when built without XTest.
bool pasteViaXTest(PasteKeys keys);

} // namespace clipitto
