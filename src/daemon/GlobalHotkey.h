#pragma once

#include <QtGlobal>

// The platform's global shortcut, one name for main.cpp: Windows' RegisterHotKey or macOS's
// Carbon hotkey. Both: setShortcut(), shortcut(), isRegistered(), signal activated().
#if defined(Q_OS_WIN)
#include "WindowsHotkey.h"
namespace clipitto {
using GlobalHotkey = WindowsHotkey;
}
#elif defined(Q_OS_MACOS)
#include "MacHotkey.h"
namespace clipitto {
using GlobalHotkey = MacHotkey;
}
#endif
