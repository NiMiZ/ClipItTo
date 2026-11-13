#pragma once

#include <QObject>

namespace clipitto {

// A system-wide shortcut on macOS (Carbon's RegisterEventHotKey, still Apple's mechanism for
// global shortcuts; it needs no permission). Same interface as WindowsHotkey.
class MacHotkey : public QObject {
    Q_OBJECT

public:
    // `id` tells several hotkeys of this process apart (the main one and the panic key).
    explicit MacHotkey(QObject* parent = nullptr, unsigned id = 1);
    ~MacHotkey() override;

    // Replaces the current shortcut; spelled like the `hotkey` config key ("Alt+Cmd+V").
    // Fails if it does not parse, names a key a Mac keyboard doesn't have, or another
    // application already owns it.
    bool setShortcut(const QString& shortcut, QString* error = nullptr);
    QString shortcut() const { return m_shortcut; }
    bool isRegistered() const { return m_hotKeyRef != nullptr; }
    // Unregisters and forgets the shortcut (the panic key turned off).
    void clear();
    unsigned id() const { return m_id; }

signals:
    void activated();

private:
    void unregister();

    QString m_shortcut;
    unsigned m_id;
    void* m_handlerRef = nullptr; // EventHandlerRef, installed once
    void* m_hotKeyRef = nullptr; // EventHotKeyRef of the registered shortcut
};

} // namespace clipitto
