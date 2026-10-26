#pragma once

#include "PasswordVault.h"

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace clipitto {

class KeyStore;

namespace db {
class Repository;
}

// Password copies for the rest of the app: the master password's vault (stored in the
// database), unlocking (Argon2id off the GUI thread) and locking again after a while, and
// the operations that need an unlock (show, paste, search inside, "not a password").
// The match key lives in the system keyring (`matchKeys`, may be null: no keyring).
class PasswordKeeper : public QObject {
    Q_OBJECT

public:
    PasswordKeeper(db::Repository& repo, std::shared_ptr<KeyStore> matchKeys, QObject* parent = nullptr);
    ~PasswordKeeper() override;

    // Reads the vault from the database and the match key from the keyring.
    void reload();
    const PasswordVault& vault() const { return m_vault; }

    // Where new password copies go when passwords_in_group is on.
    static QString passwordsGroup() { return QStringLiteral("Passwords"); }

    bool isSetUp() const { return m_vault.isSetUp(); }
    bool isUnlocked() const { return m_vault.isUnlocked(); }
    bool canMatch() const { return m_vault.canMatch(); } // false without a keyring
    QString matchProblem() const { return m_matchProblem; }
    int secondsLeft() const; // until it locks again; 0 when locked

    void setUnlockMinutes(int minutes);
    // Tests use tiny costs (also for the match hash).
    void setKdfCost(VaultKdfCost cost)
    {
        m_cost = cost;
        m_vault.setMatchCost(cost);
    }

    using Done = std::function<void(bool ok)>;
    // Derives the key off the GUI thread; `done` runs on this object's thread. Passwords are
    // taken by value: pass them with std::move, they are wiped in the worker once used.
    void unlock(QByteArray masterPassword, Done done);
    void lock();

    // Creates the vault for a first master password (and the keyring's match key).
    void setMasterPassword(QByteArray masterPassword, Done done);
    // A new master password, and a new key pair: every password copy is re-sealed. `done(false)`
    // when `oldPassword` is wrong. Windows Hello has to be turned on again afterwards.
    void changeMasterPassword(QByteArray oldPassword, QByteArray newPassword, Done done);
    // Deletes every password copy and the vault (a removed or forgotten master password).
    void reset();

    // Windows Hello: the secret key wrapped under a key from a Hello signature.
    QByteArray wrapForHello(const QByteArray& key) const { return m_vault.wrapSecret(key); }
    void setHelloWrapped(const QByteArray& wrapped); // empty: Hello off
    bool unlockWithHelloKey(const QByteArray& key);
    bool helloEnabled() const { return m_vault.stored() && !m_vault.stored()->helloWrapped.isEmpty(); }

    // Need an unlock (nullopt when locked or not a password copy).
    std::optional<QByteArray> reveal(qint64 id) const;
    std::optional<qint64> unmark(qint64 id); // the copy becomes plain text again
    // Works locked: the text is sealed to the public key. nullopt without a master password.
    std::optional<qint64> markAsPassword(qint64 id);

    // Password copies matching `typed`: part of a password while unlocked (ignoring case),
    // else only the whole password (keyed hash). Newest first.
    std::vector<qint64> search(const QString& typed) const;

signals:
    void lockChanged(bool unlocked);
    void vaultChanged(); // set up, changed or reset

private:
    void startLockTimer();
    QByteArray matchKey(bool create);

    db::Repository& m_repo;
    std::shared_ptr<KeyStore> m_matchKeys;
    PasswordVault m_vault;
    VaultKdfCost m_cost;
    QTimer m_lockTimer;
    int m_unlockMinutes = 5;
    QString m_matchProblem;
    quint64 m_generation = 0; // an unlock that finishes after a lock/reset is dropped
};

} // namespace clipitto
