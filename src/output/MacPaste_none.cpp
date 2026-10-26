#include "MacPaste.h"

namespace clipitto {

// Not macOS: nothing to remember, no permission to hold (callers only ask on a Mac).
void macRememberPasteTarget() {}

bool macPasteCommandV()
{
    return false;
}

bool macAccessibilityGranted()
{
    return false;
}

} // namespace clipitto
