#pragma once

namespace clipitto {

// Auto-paste on macOS (see SyntheticPaste.h): bring the app the user came from back to the
// front and press ⌘V in it. Posting key events needs the Accessibility permission, which the
// user grants once in System Settings › Privacy & Security.

// The frontmost app that isn't ClipItTo: call before the popup takes focus.
void macRememberPasteTarget();
// Returns false when the permission is missing (the system's prompt is shown then).
bool macPasteCommandV();
// Whether the permission is granted now (no prompt).
bool macAccessibilityGranted();

} // namespace clipitto
