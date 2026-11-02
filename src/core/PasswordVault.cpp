#include "PasswordVault.h"

#include <QRandomGenerator>
#include <QString>

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
const QByteArray kBackupContext = QByteArrayLiteral("clipitto-backup-v1");
const QByteArray kMatchContext = QByteArrayLiteral("clipitto-match-v1");

const uint8_t* bytes(const QByteArray& data)
{
    return reinterpret_cast<const uint8_t*>(data.constData());
}

uint8_t* bytes(QByteArray& data)
{
    return reinterpret_cast<uint8_t*>(data.data());
}

// Bounds for costs read back from the file: a tampered vault can't make us allocate
// gigabytes or spin for minutes.
constexpr quint32 kMaxBlocks = 1024 * 1024; // 1 GiB
constexpr quint32 kMaxPasses = 16;

QByteArray argon2id(const QByteArray& password, const QByteArray& salt, quint32 blocks, quint32 passes,
                    const QByteArray& key = {})
{
    if (blocks < 8 || passes < 1 || blocks > kMaxBlocks || passes > kMaxPasses) {
        return {};
    }
    // The work area is as large as the memory cost (64 MiB by default).
    std::unique_ptr<uint8_t[]> work(new (std::nothrow) uint8_t[size_t(blocks) * 1024]);
    if (!work) {
        return {};
    }
    crypto_argon2_config config{CRYPTO_ARGON2_ID, blocks, passes, 1};
    crypto_argon2_inputs inputs{bytes(password), bytes(salt), quint32(password.size()), quint32(salt.size())};
    crypto_argon2_extras extras = crypto_argon2_no_extras;
    if (!key.isEmpty()) {
        extras.key = bytes(key);
        extras.key_size = quint32(key.size());
    }
    QByteArray out(kKeySize, Qt::Uninitialized);
    crypto_argon2(bytes(out), kKeySize, work.get(), config, inputs, extras);
    crypto_wipe(work.get(), size_t(blocks) * 1024);
    return out;
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

// A shared secret of all zeros means the other key was a degenerate (low-order) point.
bool isZero(const uint8_t shared[32])
{
    uint8_t any = 0;
    for (int i = 0; i < 32; ++i) {
        any |= shared[i];
    }
    return any == 0;
}

QByteArray sealTo(const QByteArray& publicKey, const QByteArray& plain);
std::optional<QByteArray> openWith(const QByteArray& secret, const QByteArray& publicKey, const QByteArray& sealed);

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

QByteArray PasswordVault::deriveBackupKey(const QByteArray& password, const QByteArray& salt, KdfCost cost)
{
    return argon2id(password, salt, cost.blocks, cost.passes);
}

QByteArray PasswordVault::boxWithBackupKey(const QByteArray& key, const QByteArray& plain)
{
    return lockWith(key, plain, kBackupContext);
}

std::optional<QByteArray> PasswordVault::unboxWithBackupKey(const QByteArray& key, const QByteArray& boxed)
{
    if (key.size() != 32) {
        return std::nullopt;
    }
    return unlockWith(key, boxed, kBackupContext);
}

std::optional<PasswordVault::Rekeyed> PasswordVault::changePassword(const Stored& stored, const QByteArray& oldPassword,
                                                                    const QByteArray& newPassword, KdfCost cost)
{
    QByteArray oldKey = deriveKey(oldPassword, stored);
    std::optional<QByteArray> secret = unlockWith(oldKey, stored.wrappedSecret, kWrapContext);
    wipe(oldKey);
    if (!secret) {
        return std::nullopt;
    }
    Rekeyed rekeyed{create(newPassword, cost), *secret};
    wipe(*secret);
    return rekeyed;
}

PasswordVault::Rekeyed PasswordVault::rekeyUnlocked(const QByteArray& secret, const QByteArray& newPassword, KdfCost cost)
{
    return Rekeyed{create(newPassword, cost), secret};
}

std::optional<QByteArray> PasswordVault::reseal(const QByteArray& sealed, const QByteArray& oldSecret,
                                                const QByteArray& oldPublicKey, const QByteArray& newPublicKey)
{
    std::optional<QByteArray> plain = openWith(oldSecret, oldPublicKey, sealed);
    if (!plain) {
        return std::nullopt;
    }
    QByteArray resealed = sealTo(newPublicKey, *plain);
    wipe(*plain);
    return resealed.isEmpty() ? std::nullopt : std::optional<QByteArray>(resealed);
}

QByteArray PasswordVault::passwordBytes(QString& typed)
{
    QString normalized = typed.normalized(QString::NormalizationForm_C);
    QByteArray bytes = normalized.toUtf8();
    wipe(normalized);
    wipe(typed);
    return bytes;
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
    return m_stored ? sealTo(m_stored->publicKey, plain) : QByteArray();
}

std::optional<QByteArray> PasswordVault::open(const QByteArray& sealed) const
{
    return isUnlocked() ? openWith(m_secret, m_stored->publicKey, sealed) : std::nullopt;
}

namespace {

QByteArray sealTo(const QByteArray& publicKey, const QByteArray& plain)
{
    if (publicKey.size() != kKeySize) {
        return {};
    }
    QByteArray ephemeralSecret = PasswordVault::randomBytes(kKeySize);
    uint8_t ephemeral[32];
    crypto_x25519_public_key(ephemeral, bytes(ephemeralSecret));
    uint8_t shared[32];
    crypto_x25519(shared, bytes(ephemeralSecret), bytes(publicKey));
    PasswordVault::wipe(ephemeralSecret);
    if (isZero(shared)) {
        return {}; // not a usable public key
    }
    QByteArray key = boxKey(shared, ephemeral, bytes(publicKey));
    crypto_wipe(shared, sizeof shared);

    QByteArray out(1, kSealVersion);
    out.append(reinterpret_cast<const char*>(ephemeral), 32);
    out.append(lockWith(key, plain, kSealContext));
    PasswordVault::wipe(key);
    return out;
}

std::optional<QByteArray> openWith(const QByteArray& secret, const QByteArray& publicKey, const QByteArray& sealed)
{
    if (secret.size() != kKeySize || publicKey.size() != kKeySize || sealed.size() < 1 + 32
        || sealed.at(0) != kSealVersion) {
        return std::nullopt;
    }
    const uint8_t* ephemeral = bytes(sealed) + 1;
    uint8_t shared[32];
    crypto_x25519(shared, bytes(secret), ephemeral);
    if (isZero(shared)) {
        return std::nullopt;
    }
    QByteArray key = boxKey(shared, ephemeral, bytes(publicKey));
    crypto_wipe(shared, sizeof shared);
    std::optional<QByteArray> plain = unlockWith(key, sealed.mid(1 + 32), kSealContext);
    PasswordVault::wipe(key);
    return plain;
}

} // namespace

void PasswordVault::setMatchKeyProvider(std::function<QByteArray()> provider)
{
    wipe(m_matchKey);
    m_matchProvider = std::move(provider);
    m_matchLoaded = !m_matchProvider;
}

void PasswordVault::setMatchKey(const QByteArray& key)
{
    m_matchLoaded = true;
    wipe(m_matchKey);
    if (key.size() == kKeySize) {
        m_matchKey = key;
    }
}

QString PasswordVault::match(const QByteArray& plain) const
{
    if (!m_matchLoaded) {
        m_matchLoaded = true; // asked once; a failure is reported by canMatch() / matchProblem()
        const QByteArray key = m_matchProvider ? m_matchProvider() : QByteArray();
        if (key.size() == kKeySize) {
            m_matchKey = key;
        }
    }
    if (m_matchKey.isEmpty()) {
        return {};
    }
    // The salt is fixed (the key is the secret part), so the same password always matches.
    const QByteArray hash = argon2id(plain, QByteArray("clipitto-match-v1").leftJustified(16, '\0'),
                                     m_matchCost.blocks, m_matchCost.passes, m_matchKey);
    return hash.isEmpty() ? QString() : QStringLiteral("secret:") + QString::fromLatin1(hash.toHex());
}

} // namespace clipitto
