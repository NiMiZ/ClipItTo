#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace clipitto {

// Argon2id cost of deriving the key from the master password (outside the class: a nested
// struct with member initializers can't be a default argument inside it).
struct VaultKdfCost {
    quint32 blocks = 64 * 1024; // memory in KiB: 64 MiB
    quint32 passes = 3;
};

// The crypto behind password copies (Monocypher).
//
// Setting a master password creates an X25519 key pair. The secret key is stored encrypted
// (XChaCha20-Poly1305) under a key derived from the master password (Argon2id); the master
// password itself is never stored. New passwords are sealed to the public key, so they can be
// captured while locked; opening them needs the secret key, i.e. an unlock.
//
// A separate match key (kept in the system keyring, never in the history file) gives each
// password a keyed hash: duplicates are found and a whole password can be searched for while
// locked, without the file alone being enough to test guesses against.
class PasswordVault {
public:
    // What the database keeps (the vault table).
    struct Stored {
        QByteArray salt; // Argon2id salt, 16 bytes
        quint32 kdfBlocks = 0; // Argon2id memory, in KiB
        quint32 kdfPasses = 0;
        QByteArray publicKey; // 32 bytes
        QByteArray wrappedSecret; // nonce + MAC + encrypted secret key
        QByteArray helloWrapped; // the same secret key under a Windows Hello key; empty when off

        bool isValid() const { return salt.size() == 16 && publicKey.size() == 32 && !wrappedSecret.isEmpty(); }
    };

    using KdfCost = VaultKdfCost;

    // Creates a vault for `masterPassword`. Slow (Argon2id): keep it off the GUI thread.
    static Stored create(const QByteArray& masterPassword, KdfCost cost = {});
    // The key that unwraps the secret key. Slow (Argon2id): keep it off the GUI thread.
    static QByteArray deriveKey(const QByteArray& masterPassword, const Stored& stored);
    // A new wrapping for another master password; nullopt when `oldPassword` is wrong.
    static std::optional<Stored> changePassword(const Stored& stored, const QByteArray& oldPassword,
                                                const QByteArray& newPassword, KdfCost cost = {});
    // Wraps the secret key held by an unlocked vault under `key` (e.g. derived from a Windows
    // Hello signature). Empty when locked.
    QByteArray wrapSecret(const QByteArray& key) const;

    PasswordVault() = default;
    ~PasswordVault();
    PasswordVault(const PasswordVault&) = delete;
    PasswordVault& operator=(const PasswordVault&) = delete;

    // nullopt: no master password set (passwords are not kept).
    void load(std::optional<Stored> stored);
    const std::optional<Stored>& stored() const { return m_stored; }
    // The same vault stored differently (a new Hello wrapping): kept unlocked. False when it
    // isn't the same key pair.
    bool replaceStored(const Stored& stored);
    bool isSetUp() const { return m_stored.has_value(); }
    bool isUnlocked() const { return !m_secret.isEmpty(); }

    // With a key from deriveKey() (or a Windows Hello key and helloWrapped). False: wrong key.
    bool unlockWithKey(const QByteArray& key, bool hello = false);
    void lock(); // wipes the secret key

    // Seals `plain` to the public key; works while locked. Empty when not set up.
    QByteArray seal(const QByteArray& plain) const;
    // nullopt when locked or when `sealed` is damaged.
    std::optional<QByteArray> open(const QByteArray& sealed) const;

    void setMatchKey(const QByteArray& key); // 32 bytes; empty: no keyring, no matching
    bool canMatch() const { return !m_matchKey.isEmpty(); }
    // "secret:<hex>": keyed hash of `plain`; empty without a match key.
    QString match(const QByteArray& plain) const;

    static QByteArray randomBytes(int size);
    static void wipe(QByteArray& secret);
    static void wipe(QString& secret);

private:
    std::optional<Stored> m_stored;
    QByteArray m_secret; // the X25519 secret key while unlocked
    QByteArray m_matchKey;
};

} // namespace clipitto
