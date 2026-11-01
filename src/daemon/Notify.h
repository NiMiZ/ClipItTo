#pragma once

#include <QString>

class QSystemTrayIcon;

namespace clipitto {

// A side notification that needs no window of ours and nothing from the network. Linux with
// D-Bus: the freedesktop notifications service (GNOME has no tray icon). Elsewhere, or when that
// service is missing: Qt's tray icon `showMessage` where a tray exists (Windows, macOS).
// Returns false when neither could be used; the caller then shows the text in the popup.
// The session's one tray icon (main.cpp owns it); its messages are used on Windows and macOS.
// Not owned here; forgotten when it is destroyed.
void setNotifyTray(QSystemTrayIcon* tray);

bool desktopNotify(const QString& title, const QString& body);

} // namespace clipitto
