#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>

namespace clipitto {

// A global shortcut in Win32 terms (RegisterHotKey): MOD_* flags and a VK_* key code, plus
// the same shortcut for macOS (Carbon RegisterEventHotKey: modifier masks and a virtual key
// code by physical key position on a US keyboard).
struct NativeHotkey {
    quint32 modifiers = 0;
    quint32 virtualKey = 0;
    quint32 macModifiers = 0; // Carbon: cmdKey 0x100, shiftKey 0x200, optionKey 0x800, controlKey 0x1000
    quint32 macKeyCode = kNoMacKey; // kVK_*; kNoMacKey for a key a Mac keyboard doesn't have
    static constexpr quint32 kNoMacKey = 0xFFFF;
    QString canonical; // normalized spelling, e.g. "Ctrl+Alt+V"
    QString gtkAccelerator; // the same shortcut for GNOME settings, e.g. "<Control><Alt>v"
};

// Parses "Ctrl+Alt+V", "win+shift+f9", "Ctrl+Space", ... (case-insensitive, '+'-separated).
// Win, Super, Meta and Cmd are one modifier: the Windows key, and ⌘ on a Mac (spelled Cmd
// there, Win elsewhere, in `canonical`); Alt is ⌥, Ctrl is ⌃.
// At least one modifier is required. Keys: A-Z, 0-9, F1-F24, Space, Tab, Enter, Insert,
// Delete, Home, End, PageUp, PageDown and the US-layout punctuation keys ` - = [ ] \ ; ' , . /
// (Ctrl+` is the key left of 1). Returns nullopt for anything else.
std::optional<NativeHotkey> parseHotkey(const QString& text);

namespace win32 { // values from WinUser.h, so this stays portable and testable
inline constexpr quint32 ModAlt = 0x0001;
inline constexpr quint32 ModControl = 0x0002;
inline constexpr quint32 ModShift = 0x0004;
inline constexpr quint32 ModWin = 0x0008;
inline constexpr quint32 ModNoRepeat = 0x4000;
} // namespace win32

} // namespace clipitto
