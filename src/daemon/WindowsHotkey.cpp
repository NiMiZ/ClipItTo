#include "WindowsHotkey.h"

#include "Hotkey.h"

#include <QCoreApplication>

#include <windows.h>

namespace clipitto {

namespace {

} // namespace

WindowsHotkey::WindowsHotkey(QObject* parent, int id) : QObject(parent), m_id(id)
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
    if (!RegisterHotKey(nullptr, m_id, parsed->modifiers | win32::ModNoRepeat, parsed->virtualKey)) {
        if (error) {
            *error = QStringLiteral("%1 is already used by another application").arg(m_shortcut);
        }
        return false;
    }
    m_registered = true;
    return true;
}

void WindowsHotkey::clear()
{
    unregister();
    m_shortcut.clear();
}

void WindowsHotkey::unregister()
{
    if (m_registered) {
        UnregisterHotKey(nullptr, m_id);
        m_registered = false;
    }
}

bool WindowsHotkey::nativeEventFilter(const QByteArray& eventType, void* message, qintptr*)
{
    if (eventType != "windows_dispatcher_MSG" && eventType != "windows_generic_MSG") {
        return false;
    }
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == WPARAM(m_id)) {
        emit activated();
        return true;
    }
    return false;
}

} // namespace clipitto
