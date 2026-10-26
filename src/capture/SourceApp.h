#pragma once

#include <QString>

namespace clipitto::capture {

// The name of the app that owns the clipboard now (e.g. "KeePassXC"), for a password copy's
// default note. Asked only for copies marked by a password manager.
//   Windows: the clipboard owner window's process (file description, else the exe name)
//   X11:     the CLIPBOARD selection owner's WM_CLASS, else the active window's
//   elsewhere (Wayland without the GNOME extension): unknown, an empty string
QString clipboardSourceApp();

} // namespace clipitto::capture
