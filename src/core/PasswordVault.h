#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <functional>
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
    // A new master password with a NEW key pair: whoever had the old one (and an old copy of
    // the file) can't open what is sealed from now on. The caller re-seals every password
    // from the old secret key to the new public key (reseal()) and saves `stored` with them.
    // nullopt when `oldPassword` is wrong. The Hello wrapping is dropped (set it up again).
    struct Rekeyed {
        Stored stored;
        QByteArray oldSecret; // wipe after re-sealing
    };
    static std::optional<Rekeyed> changePassword(const Stored& stored, const QByteArray& oldPassword,
                                                 const QByteArray& newPassword, KdfCost cost = {});
    // The same for a vault that was unlocked without the old password (Windows Hello): the new
    // key pair is made from `secret`, the unlocked secret key (see secretCopy()).
    static Rekeyed rekeyUnlocked(const QByteArray& secret, const QByteArray& newPassword, KdfCost cost = {});
    // Opens `sealed` with `oldSecret` (and its public key) and seals it to `newPublicKey`.
    static std::optional<QByteArray> reseal(const QByteArray& sealed, const QByteArray& oldSecret,
                                            const QByteArray& oldPublicKey, const QByteArray& newPublicKey);
    // What a typed master password is turned into: Unicode NFC (so input methods that compose
    // characters differently unlock alike), UTF-8. Wipes `typed`.
    static QByteArray passwordBytes(QString& typed);
    // Wraps the secret key held by an unlocked vault under `key` (e.g. derived from a Windows
    // Hello signature). Empty when locked.
    QByteArray wrapSecret(const QByteArray& key) const;

    // Passwords in a backup file: encrypted under a key from a password (Argon2id), not under
    // this machine's vault, so any machine with the password can open them. Slow (Argon2id).
    // The cost is bounded like the vault's: an empty key for a cost that is out of range.
    static QByteArray deriveBackupKey(const QByteArray& password, const QByteArray& salt, KdfCost cost);
    static QByteArray boxWithBackupKey(const QByteArray& key, const QByteArray& plain);
    static std::optional<QByteArray> unboxWithBackupKey(const QByteArray& key, const QByteArray& boxed);

    // The sync files (Sync.h): a key from the sync passphrase (deriveBackupKey does that), and boxes with a context of
    // their own, so a sync box can't be passed off as a backup box.
    static QByteArray boxWithSyncKey(const QByteArray& key, const QByteArray& plain, const QByteArray& header);
    static std::optional<QByteArray> unboxWithSyncKey(const QByteArray& key, const QByteArray& boxed, const QByteArray& header);

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
    // A copy of the secret key while unlocked (empty when locked); wipe it after use.
    QByteArray secretCopy() const { return m_secret; }

    // Seals `plain` to the public key; works while locked. Empty when not set up.
    QByteArray seal(const QByteArray& plain) const;
    // nullopt when locked or when `sealed` is damaged.
    std::optional<QByteArray> open(const QByteArray& sealed) const;

    void setMatchKey(const QByteArray& key); // 32 bytes; empty: no keyring, no matching
    // The key is fetched from the keyring (by `provider`) the first time it is needed, not at start, so a locked
    // keyring is not asked about before a password is copied or searched. A null provider: no key until set.
    void setMatchKeyProvider(std::function<QByteArray()> provider);
    bool matchKeyLoaded() const { return m_matchLoaded; }
    // True while the key has not been asked for yet (it may turn out to be missing).
    bool canMatch() const { return !m_matchKey.isEmpty() || !m_matchLoaded; }
    // "secret:<hex>": a memory-hard keyed hash of `plain` (Argon2id keyed with the match key),
    // so even with the keyring at hand every guess costs real memory and time. Empty without
    // a match key.
    QString match(const QByteArray& plain) const;
    void setMatchCost(VaultKdfCost cost) { m_matchCost = cost; } // tests use a tiny one

    static QByteArray randomBytes(int size);
    static void wipe(QByteArray& secret);
    static void wipe(QString& secret);

private:
    std::optional<Stored> m_stored;
    QByteArray m_secret; // the X25519 secret key while unlocked
    mutable QByteArray m_matchKey; // loaded on first use when there is a provider
    mutable bool m_matchLoaded = true;
    std::function<QByteArray()> m_matchProvider;
    VaultKdfCost m_matchCost{16 * 1024, 2}; // 16 MiB: ~20 ms per capture or search in a release build
};

} // namespace clipitto
