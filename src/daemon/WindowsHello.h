#pragma once

#include <QByteArray>
#include <QString>

// Windows Hello as a second way to unlock password copies, chosen in CMake:
//   Windows:   KeyCredentialManager (C++/WinRT)  - WindowsHello_win.cpp
//   elsewhere: explicitly unsupported            - WindowsHello_none.cpp
// Every call blocks (Windows shows its own prompt) and must not run on the GUI thread.
namespace clipitto::hello {

// Whether Windows Hello is set up and usable here; false with the reason otherwise.
bool available(QString* why = nullptr);
// ClipItTo's Windows Hello key signs a fixed challenge; the result, hashed, is a 32-byte key
// that wraps the vault's secret key. `create` makes a new Hello key (when turning it on);
// otherwise the existing one is used. Empty with `error` when cancelled or unavailable.
QByteArray unlockKey(bool create, QString* error = nullptr);
// Deletes ClipItTo's Windows Hello key (turning it off, or a removed master password).
void removeKey();
// Cancels a Windows Hello prompt that is waiting for the user (quitting), so the blocked
// unlockKey() call returns right away. Any thread.
void cancelPending();

} // namespace clipitto::hello
