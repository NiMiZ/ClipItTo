#pragma once

namespace clipitto::ui {

// macOS only (no-ops elsewhere, so callers need no #ifdef).
//
// ClipItTo lives in the menu bar: no Dock icon, no app menu. The activation policy is set at
// run time, so it holds for a bare binary as well as for the .app bundle.
void makeBackgroundApp();
// A background app's window doesn't get keyboard focus by itself: bring the app to the front
// so the popup can be typed into (the previous app is activated again when pasting).
void bringAppToFront();

} // namespace clipitto::ui
