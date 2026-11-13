#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

namespace clipitto {

// A system-wide shortcut on Windows (RegisterHotKey). WM_HOTKEY arrives as a thread
// message, which Qt hands to native event filters. Pressing the hotkey also grants this
// process the right to bring its popup to the foreground.
class WindowsHotkey : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    // `id` tells several hotkeys of this process apart (the main one and the panic key).
    explicit WindowsHotkey(QObject* parent = nullptr, int id = 0xC11);
    ~WindowsHotkey() override;

    // Replaces the current shortcut; spelled like the `hotkey` config key ("Ctrl+Alt+V").
    // Fails if it does not parse or another application already owns it.
    bool setShortcut(const QString& shortcut, QString* error = nullptr);
    QString shortcut() const { return m_shortcut; }
    bool isRegistered() const { return m_registered; }
    // Unregisters and forgets the shortcut (the panic key turned off).
    void clear();

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void activated();

private:
    void unregister();

    QString m_shortcut;
    bool m_registered = false;
    int m_id;
};

} // namespace clipitto
