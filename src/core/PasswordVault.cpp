#include "PasswordVault.h"

#include <QRandomGenerator>

#include <monocypher.h>

#include <cstring>
#include <memory>
#include <new>

namespace clipitto {

namespace {

constexpr int kKeySize = 32;
constexpr int kNonceSize = 24;
constexpr int kMacSize = 16;
constexpr char kSealVersion = 1;
// Bound to every wrapped key and sealed password, so one can't be passed off as the other.
const QByteArray kWrapContext = QByteArrayLiteral("clipitto-vault-v1");
const QByteArray kSealContext = QByteArrayLiteral("clipitto-seal-v1");
const QByteArray kMatchContext = QByteArrayLiteral("clipitto-match-v1");

const uint8_t* bytes(const QByteArray& data)
{
    return reinterpret_cast<const uint8_t*>(data.constData());
}

uint8_t* bytes(QByteArray& data)
{
    return reinterpret_cast<uint8_t*>(data.data());
}

QByteArray argon2id(const QByteArray& password, const QByteArray& salt, quint32 blocks, quint32 passes)
{
    if (blocks < 8 || passes < 1) {
        return {};
    }
    // The work area is as large as the memory cost (64 MiB by default).
    std::unique_ptr<uint8_t[]> work(new (std::nothrow) uint8_t[size_t(blocks) * 1024]);
    if (!work) {
        return {};
    }
    QByteArray key(kKeySize, Qt::Uninitialized);
    crypto_argon2_config config{CRYPTO_ARGON2_ID, blocks, passes, 1};
    crypto_argon2_inputs inputs{bytes(password), bytes(salt), quint32(password.size()), quint32(salt.size())};
    crypto_argon2(bytes(key), kKeySize, work.get(), config, inputs, crypto_argon2_no_extras);
    crypto_wipe(work.get(), size_t(blocks) * 1024);
    return key;
}

// nonce | MAC | ciphertext of `plain` under `key`.
QByteArray lockWith(const QByteArray& key, const QByteArray& plain, const QByteArray& context)
{
    QByteArray out = PasswordVault::randomBytes(kNonceSize);
    out.resize(kNonceSize + kMacSize + plain.size());
    uint8_t* nonce = bytes(out);
    crypto_aead_lock(nonce + kNonceSize + kMacSize, nonce + kNonceSize, bytes(key), nonce, bytes(context),
                     size_t(context.size()), bytes(plain), size_t(plain.size()));
    return out;
}

std::optional<QByteArray> unlockWith(const QByteArray& key, const QByteArray& locked, const QByteArray& context)
{
    if (key.size() != kKeySize || locked.size() < kNonceSize + kMacSize) {
        return std::nullopt;
    }
    const uint8_t* nonce = bytes(locked);
    QByteArray plain(locked.size() - kNonceSize - kMacSize, Qt::Uninitialized);
    if (crypto_aead_unlock(bytes(plain), nonce + kNonceSize, bytes(key), nonce, bytes(context), size_t(context.size()),
                           nonce + kNonceSize + kMacSize, size_t(plain.size()))
        != 0) {
        return std::nullopt;
    }
    return plain;
}

// The symmetric key for one sealed box: BLAKE2b(shared secret | ephemeral pk | recipient pk).
QByteArray boxKey(const uint8_t shared[32], const uint8_t ephemeral[32], const uint8_t recipient[32])
{
    QByteArray key(kKeySize, Qt::Uninitialized);
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, kKeySize);
    crypto_blake2b_update(&ctx, shared, 32);
    crypto_blake2b_update(&ctx, ephemeral, 32);
    crypto_blake2b_update(&ctx, recipient, 32);
    crypto_blake2b_final(&ctx, bytes(key));
    return key;
}

} // namespace

QByteArray PasswordVault::randomBytes(int size)
{
    QByteArray out(size, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(out.data()),
                                         reinterpret_cast<quint32*>(out.data()) + size / 4);
    for (int i = size / 4 * 4; i < size; ++i) {
        out[i] = char(QRandomGenerator::system()->generate());
    }
    return out;
}

void PasswordVault::wipe(QByteArray& secret)
{
    if (!secret.isEmpty()) {
        crypto_wipe(secret.data(), size_t(secret.size())); // detaches first if shared
    }
    secret.clear();
}

void PasswordVault::wipe(QString& secret)
{
    if (!secret.isEmpty()) {
        crypto_wipe(secret.data(), size_t(secret.size()) * sizeof(QChar));
    }
    secret.clear();
}

PasswordVault::Stored PasswordVault::create(const QByteArray& masterPassword, KdfCost cost)
{
    Stored stored;
    stored.salt = randomBytes(16);
    stored.kdfBlocks = cost.blocks;
    stored.kdfPasses = cost.passes;
    QByteArray secret = randomBytes(kKeySize);
    stored.publicKey.resize(kKeySize);
    crypto_x25519_public_key(bytes(stored.publicKey), bytes(secret));
    QByteArray key = deriveKey(masterPassword, stored);
    stored.wrappedSecret = lockWith(key, secret, kWrapContext);
    wipe(key);
    wipe(secret);
    return stored;
}

QByteArray PasswordVault::deriveKey(const QByteArray& masterPassword, const Stored& stored)
{
    return argon2id(masterPassword, stored.salt, stored.kdfBlocks, stored.kdfPasses);
}

std::optional<PasswordVault::Stored> PasswordVault::changePassword(const Stored& stored, const QByteArray& oldPassword,
                                                                   const QByteArray& newPassword, KdfCost cost)
{
    QByteArray oldKey = deriveKey(oldPassword, stored);
    std::optional<QByteArray> secret = unlockWith(oldKey, stored.wrappedSecret, kWrapContext);
    wipe(oldKey);
    if (!secret) {
        return std::nullopt;
    }
    Stored changed = stored;
    changed.salt = randomBytes(16);
    changed.kdfBlocks = cost.blocks;
    changed.kdfPasses = cost.passes;
    QByteArray newKey = deriveKey(newPassword, changed);
    changed.wrappedSecret = lockWith(newKey, *secret, kWrapContext);
    wipe(newKey);
    wipe(*secret);
    return changed;
}

QByteArray PasswordVault::wrapSecret(const QByteArray& key) const
{
    if (!isUnlocked() || key.size() != kKeySize) {
        return {};
    }
    return lockWith(key, m_secret, kWrapContext);
}

PasswordVault::~PasswordVault()
{
    lock();
    wipe(m_matchKey);
}

void PasswordVault::load(std::optional<Stored> stored)
{
    lock();
    m_stored = std::move(stored);
}

bool PasswordVault::replaceStored(const Stored& stored)
{
    if (!m_stored || stored.publicKey != m_stored->publicKey) {
        return false;
    }
    m_stored = stored;
    return true;
}

bool PasswordVault::unlockWithKey(const QByteArray& key, bool hello)
{
    if (!m_stored) {
        return false;
    }
    std::optional<QByteArray> secret =
        unlockWith(key, hello ? m_stored->helloWrapped : m_stored->wrappedSecret, kWrapContext);
    if (!secret || secret->size() != kKeySize) {
        return false;
    }
    lock();
    m_secret = *secret;
    wipe(*secret);
    return true;
}

void PasswordVault::lock()
{
    wipe(m_secret);
}

QByteArray PasswordVault::seal(const QByteArray& plain) const
{
    if (!m_stored) {
        return {};
    }
    QByteArray ephemeralSecret = randomBytes(kKeySize);
    uint8_t ephemeral[32];
    crypto_x25519_public_key(ephemeral, bytes(ephemeralSecret));
    uint8_t shared[32];
    crypto_x25519(shared, bytes(ephemeralSecret), bytes(m_stored->publicKey));
    QByteArray key = boxKey(shared, ephemeral, bytes(m_stored->publicKey));
    crypto_wipe(shared, sizeof shared);
    wipe(ephemeralSecret);

    QByteArray out(1, kSealVersion);
    out.append(reinterpret_cast<const char*>(ephemeral), 32);
    out.append(lockWith(key, plain, kSealContext));
    wipe(key);
    return out;
}

std::optional<QByteArray> PasswordVault::open(const QByteArray& sealed) const
{
    if (!isUnlocked() || sealed.size() < 1 + 32 || sealed.at(0) != kSealVersion) {
        return std::nullopt;
    }
    const uint8_t* ephemeral = bytes(sealed) + 1;
    uint8_t shared[32];
    crypto_x25519(shared, bytes(m_secret), ephemeral);
    QByteArray key = boxKey(shared, ephemeral, bytes(m_stored->publicKey));
    crypto_wipe(shared, sizeof shared);
    std::optional<QByteArray> plain = unlockWith(key, sealed.mid(1 + 32), kSealContext);
    wipe(key);
    return plain;
}

void PasswordVault::setMatchKey(const QByteArray& key)
{
    wipe(m_matchKey);
    if (key.size() == kKeySize) {
        m_matchKey = key;
    }
}

QString PasswordVault::match(const QByteArray& plain) const
{
    if (m_matchKey.isEmpty()) {
        return {};
    }
    QByteArray hash(kKeySize, Qt::Uninitialized);
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, kKeySize, bytes(m_matchKey), size_t(m_matchKey.size()));
    crypto_blake2b_update(&ctx, bytes(kMatchContext), size_t(kMatchContext.size()));
    crypto_blake2b_update(&ctx, bytes(plain), size_t(plain.size()));
    crypto_blake2b_final(&ctx, bytes(hash));
    return QStringLiteral("secret:") + QString::fromLatin1(hash.toHex());
}

} // namespace clipitto
