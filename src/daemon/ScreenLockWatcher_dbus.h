#pragma once

#include "ScreenLockWatcher.h"

#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QList>

namespace clipitto {

// Linux: reports a locked screen or a sleeping PC from
//   - systemd-logind (system bus): the Lock signal of the user's own session, and Manager.PrepareForSleep(true);
//   - the screensavers (session bus): org.gnome.ScreenSaver / org.freedesktop.ScreenSaver ActiveChanged(true).
// A bus or a service that is not there is skipped with a debug line. The unlock signals are ignored on purpose.
// The buses are parameters so that a test can play all of them on one private bus.
class DbusScreenLockWatcher : public ScreenLockWatcher {
    Q_OBJECT

public:
    // `sessionPath`: logind's object path of the user's session; empty = find it (by process id, then XDG_SESSION_ID).
    DbusScreenLockWatcher(const QDBusConnection& systemBus, const QDBusConnection& sessionBus,
                          const QString& sessionPath = QString(), QObject* parent = nullptr);
    ~DbusScreenLockWatcher() override;

    bool start(QString* why = nullptr) override;
    void stop() override;
    bool isWatching() const override { return m_watching; }

private slots:
    // Slots named by the D-Bus signals they receive (QtDBus matches the signature).
    void onSessionLock();
    void onPrepareForSleep(bool starting);
    void onScreenSaverActive(bool active);
    void onSessionPathFound(QDBusPendingCallWatcher* call);

private:
    struct Subscription {
        QDBusConnection bus;
        QString service;
        QString path;
        QString interface;
        QString name;
        QString slot;
    };
    bool subscribe(const QDBusConnection& bus, const QString& service, const QString& path, const QString& interface,
                   const QString& name, const char* slot);
    void subscribeSession(const QString& path);
    void lookUpSession(bool byPid);

    QDBusConnection m_system;
    QDBusConnection m_sessionBus;
    QString m_sessionPath;
    QList<Subscription> m_subscriptions;
    bool m_watching = false;
    quint64 m_epoch = 0; // answers of a lookup made before stop() are dropped
};

} // namespace clipitto
