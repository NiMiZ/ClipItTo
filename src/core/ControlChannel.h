#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace clipitto {

// How the outside world drives the daemon: requests from shortcuts / the CLI / capture
// helpers come in as signals; status and popup visibility go out.
// Platform implementations: ipc::DbusControlChannel (Linux), localipc::LocalControlChannel
// (Windows; local socket / named pipe).
class ControlChannel : public QObject {
    Q_OBJECT

public:
    enum class StartResult { Ok, AlreadyRunning, Failed };

    using QObject::QObject;

    // Claims single-instance ownership and starts accepting requests.
    virtual StartResult start(QString* error = nullptr) = 0;

    void setStatusProvider(std::function<QVariantMap()> provider) { m_statusProvider = std::move(provider); }
    QVariantMap status() const { return m_statusProvider ? m_statusProvider() : QVariantMap{}; }

    // Whether the daemon keeps password copies now; a capture helper reads a copy marked by a
    // password manager only then (WantsPasswords).
    void setPasswordsWantedProvider(std::function<bool()> provider) { m_passwordsWanted = std::move(provider); }
    bool passwordsWanted() const { return m_passwordsWanted && m_passwordsWanted(); }

    // The self-test (clipitto --selftest, roadmap 31): the daemon runs its checks and calls `reply` with the report when they
    // are done, which may be many seconds later (the channel answers the caller then, not before). The map holds "report" (the
    // text) and the counts "pass", "warn", "fail", "skipped".
    using SelfTestReply = std::function<void(const QVariantMap&)>;
    void setSelfTestProvider(std::function<void(SelfTestReply)> provider) { m_selfTest = std::move(provider); }
    void runSelfTest(SelfTestReply reply)
    {
        if (m_selfTest) {
            m_selfTest(std::move(reply));
        } else if (reply) {
            reply(QVariantMap{{QStringLiteral("error"), QStringLiteral("the self-test is not available")}});
        }
    }

signals:
    void toggleRequested(const QString& activationToken);
    void showRequested(const QString& activationToken);
    void hideRequested();
    // `html`: the copy's formatted version when the caller has one (empty otherwise).
    void ingestRequested(const QString& text, const QStringList& mimeTypes, const QString& html = QString());
    // An encoded picture (`mimeType`, e.g. image/png) from a capture helper.
    void ingestImageRequested(const QByteArray& data, const QString& mimeType, const QStringList& mimeTypes);
    void reloadRequested();
    void lockPasswordsRequested(); // clipitto --lock
    void syncRequested(); // clipitto --sync: one sync run through the shared folder
    // clipitto --panic (or the panic shortcut's command): wipe the history, lock the passwords, empty the clipboard.
    // Connected directly, so the reply goes out after it is done.
    void panicRequested();
    void quitRequested();

    // Emitted by the daemon when the popup is shown or hidden; implementations forward it to clients.
    void visibilityChanged(bool visible);

private:
    std::function<QVariantMap()> m_statusProvider;
    std::function<bool()> m_passwordsWanted;
    std::function<void(SelfTestReply)> m_selfTest;
};

} // namespace clipitto
