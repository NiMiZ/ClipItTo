#include "WindowsHotkey.h"

#include "Hotkey.h"

#include <QCoreApplication>

#include <windows.h>

namespace clipitto {

namespace {

constexpr int kHotkeyId = 0xC11; // any id unique within this thread

} // namespace

WindowsHotkey::WindowsHotkey(QObject* parent) : QObject(parent)
{
    QCoreApplication::instance()->installNativeEventFilter(this);
}

WindowsHotkey::~WindowsHotkey()
{
    unregister();
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

bool WindowsHotkey::setShortcut(const QString& shortcut, QString* error)
{
    unregister();
    const auto parsed = parseHotkey(shortcut);
    if (!parsed) {
        if (error) {
            *error = QStringLiteral("not a valid shortcut: %1").arg(shortcut);
        }
        return false;
    }
    m_shortcut = parsed->canonical;
    // Registered for this (the GUI) thread: no window needed, WM_HOTKEY is a thread message.
    if (!RegisterHotKey(nullptr, kHotkeyId, parsed->modifiers | win32::ModNoRepeat, parsed->virtualKey)) {
        if (error) {
            *error = QStringLiteral("%1 is already used by another application").arg(m_shortcut);
        }
        return false;
    }
    m_registered = true;
    return true;
}

void WindowsHotkey::unregister()
{
    if (m_registered) {
        UnregisterHotKey(nullptr, kHotkeyId);
        m_registered = false;
    }
}

bool WindowsHotkey::nativeEventFilter(const QByteArray& eventType, void* message, qintptr*)
{
    if (eventType != "windows_dispatcher_MSG" && eventType != "windows_generic_MSG") {
        return false;
    }
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == WPARAM(kHotkeyId)) {
        emit activated();
        return true;
    }
    return false;
}

} // namespace clipitto
