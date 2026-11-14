#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace clipitto {

// Where the history encryption key lives: the system keyring in production
// (KeychainKeyStore), an in-memory fake in tests. Calls block until done.
class KeyStore {
public:
    virtual ~KeyStore() = default;

    // False (with the reason) when no keyring service can be reached.
    virtual bool available(QString* why = nullptr) = 0;
    // nullopt with an empty error: no key stored yet. nullopt with an error: lookup failed.
    virtual std::optional<QByteArray> read(QString* error = nullptr) = 0;
    virtual bool write(const QByteArray& key, QString* error = nullptr) = 0;
    virtual bool remove(QString* error = nullptr) = 0;
    // True when a call may show the user a window (a keyring to unlock, a Keychain prompt). ClipItTo then says why
    // first (AnnouncingKeyStore). False where it never does: Windows' Credential Manager.
    virtual bool mayAsk() const { return true; }
};

// Tells the user why before the keyring is asked: the keyring may show its own "unlock" window, and without a
// word from ClipItTo nobody knows what wanted it. `announce(purpose)` runs before each call (the receiver
// limits how often it speaks); the call itself goes to `inner`.
class AnnouncingKeyStore : public KeyStore {
public:
    AnnouncingKeyStore(std::shared_ptr<KeyStore> inner, QString purpose,
                       std::shared_ptr<std::function<void(const QString&)>> announce)
        : m_inner(std::move(inner)), m_purpose(std::move(purpose)), m_announce(std::move(announce))
    {
    }
    bool available(QString* why = nullptr) override { return m_inner->available(why); } // only a probe: no word
    bool mayAsk() const override { return m_inner->mayAsk(); }
    std::optional<QByteArray> read(QString* error = nullptr) override { say(); return m_inner->read(error); }
    bool write(const QByteArray& key, QString* error = nullptr) override { say(); return m_inner->write(key, error); }
    bool remove(QString* error = nullptr) override { say(); return m_inner->remove(error); }

private:
    void say() const
    {
        if (m_announce && *m_announce && m_inner->mayAsk()) {
            (*m_announce)(m_purpose);
        }
    }
    std::shared_ptr<KeyStore> m_inner;
    QString m_purpose;
    std::shared_ptr<std::function<void(const QString&)>> m_announce;
};

} // namespace clipitto
