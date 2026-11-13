// Apple's headers first: Qt's keyword macros (signals, slots, emit) must not meet them.
#import <Carbon/Carbon.h>

#include "MacHotkey.h"

#include "Hotkey.h"

#include <QMetaObject>

namespace clipitto {

namespace {

constexpr OSType kSignature = 'CLIP';

OSStatus onHotKey(EventHandlerCallRef, EventRef event, void* userData)
{
    EventHotKeyID id;
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof id, nullptr, &id) == noErr
        && id.signature == kSignature) {
        auto* self = static_cast<MacHotkey*>(userData);
        if (id.id != self->id()) {
            return eventNotHandledErr; // another of our hotkeys: its own handler takes it
        }
        // Queued: the popup is shown from the event loop, not from inside Carbon's dispatch.
        QMetaObject::invokeMethod(self, [self] { Q_EMIT self->activated(); }, Qt::QueuedConnection);
        return noErr;
    }
    return eventNotHandledErr;
}

} // namespace

MacHotkey::MacHotkey(QObject* parent, unsigned id) : QObject(parent), m_id(id)
{
    const EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
    EventHandlerRef handler = nullptr;
    if (InstallApplicationEventHandler(&onHotKey, 1, &spec, this, &handler) == noErr) {
        m_handlerRef = handler;
    }
}

MacHotkey::~MacHotkey()
{
    unregister();
    if (m_handlerRef) {
        RemoveEventHandler(static_cast<EventHandlerRef>(m_handlerRef));
    }
}

bool MacHotkey::setShortcut(const QString& shortcut, QString* error)
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
    if (parsed->macKeyCode == NativeHotkey::kNoMacKey) {
        if (error) {
            *error = QStringLiteral("%1: that key is not on a Mac keyboard").arg(m_shortcut);
        }
        return false;
    }
    if (!m_handlerRef) {
        if (error) {
            *error = QStringLiteral("could not listen for shortcuts");
        }
        return false;
    }
    EventHotKeyRef ref = nullptr;
    const OSStatus status = RegisterEventHotKey(parsed->macKeyCode, parsed->macModifiers,
                                                EventHotKeyID{kSignature, m_id}, GetApplicationEventTarget(), 0,
                                                &ref);
    if (status != noErr) {
        if (error) {
            *error = status == eventHotKeyExistsErr
                ? QStringLiteral("%1 is already used by another application").arg(m_shortcut)
                : QStringLiteral("%1 could not be registered (error %2)").arg(m_shortcut).arg(int(status));
        }
        return false;
    }
    m_hotKeyRef = ref;
    return true;
}

void MacHotkey::clear()
{
    unregister();
    m_shortcut.clear();
}

void MacHotkey::unregister()
{
    if (m_hotKeyRef) {
        UnregisterEventHotKey(static_cast<EventHotKeyRef>(m_hotKeyRef));
        m_hotKeyRef = nullptr;
    }
}

} // namespace clipitto
