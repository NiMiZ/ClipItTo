#include "PasswordKeeper.h"

#include "KeyStore.h"
#include "db/Repository.h"
#include "db/Sqlite.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QThreadPool>

#include <utility>

Q_LOGGING_CATEGORY(lcPasswords, "clipitto.passwords")

namespace clipitto {

namespace {

constexpr int kKeySize = 32;

// Runs `work` on the global pool and hands its result to `then` on `context`'s thread,
// unless `context` is gone by then.
template <typename Result>
void runOffThread(QObject* context, std::function<Result()> work, std::function<void(Result)> then)
{
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start([guard, work = std::move(work), then = std::move(then)]() mutable {
        Result result = work();
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(guard.data(), [guard, then = std::move(then), result = std::move(result)]() mutable {
            if (guard) {
                then(std::move(result));
            }
        });
    });
}

} // namespace

PasswordKeeper::PasswordKeeper(db::Repository& repo, std::shared_ptr<KeyStore> matchKeys, QObject* parent)
    : QObject(parent)
    , m_repo(repo)
    , m_matchKeys(std::move(matchKeys))
{
    m_lockTimer.setSingleShot(true);
    connect(&m_lockTimer, &QTimer::timeout, this, &PasswordKeeper::lock);
}

PasswordKeeper::~PasswordKeeper() = default;

void PasswordKeeper::reload()
{
    const bool wasUnlocked = isUnlocked();
    try {
        m_vault.load(m_repo.loadVault());
    } catch (const db::DbError& e) {
        qCWarning(lcPasswords) << "could not read the password vault:" << e.what();
        m_vault.load(std::nullopt);
    }
    ++m_generation;
    m_lockTimer.stop();
    m_vault.setMatchKey(isSetUp() ? matchKey(false) : QByteArray());
    if (wasUnlocked) {
        emit lockChanged(false);
    }
}

QByteArray PasswordKeeper::matchKey(bool create)
{
    m_matchProblem.clear();
    QString error;
    if (!m_matchKeys) {
        m_matchProblem = QStringLiteral("no system keyring");
        return {};
    }
    // No separate availability probe: each keyring job may wait (Linux), so one read says it.
    std::optional<QByteArray> key = m_matchKeys->read(&error);
    if (key && key->size() == kKeySize) {
        return *key;
    }
    if (!error.isEmpty()) {
        m_matchProblem = error;
        return {};
    }
    if (!create) {
        m_matchProblem = QStringLiteral("the keyring has no match key");
        return {};
    }
    QByteArray fresh = PasswordVault::randomBytes(kKeySize);
    if (!m_matchKeys->write(fresh, &error)) {
        m_matchProblem = error;
        return {};
    }
    return fresh;
}

int PasswordKeeper::secondsLeft() const
{
    return isUnlocked() && m_lockTimer.isActive() ? qMax(1, m_lockTimer.remainingTime() / 1000) : 0;
}

void PasswordKeeper::setUnlockMinutes(int minutes)
{
    m_unlockMinutes = qMax(1, minutes);
}

void PasswordKeeper::startLockTimer()
{
    m_lockTimer.start(m_unlockMinutes * 60 * 1000);
}

void PasswordKeeper::unlock(QByteArray masterPassword, Done done)
{
    if (!m_vault.stored()) {
        PasswordVault::wipe(masterPassword);
        done(false);
        return;
    }
    const PasswordVault::Stored stored = *m_vault.stored();
    const quint64 generation = m_generation;
    runOffThread<QByteArray>(
        this,
        [password = std::move(masterPassword), stored]() mutable {
            QByteArray key = PasswordVault::deriveKey(password, stored);
            PasswordVault::wipe(password);
            return key;
        },
        [this, generation, done = std::move(done)](QByteArray key) {
            const bool ok = generation == m_generation && m_vault.unlockWithKey(key);
            PasswordVault::wipe(key);
            if (ok) {
                startLockTimer();
                emit lockChanged(true);
            }
            done(ok);
        });
}

void PasswordKeeper::lock()
{
    m_lockTimer.stop();
    ++m_generation;
    if (isUnlocked()) {
        m_vault.lock();
        emit lockChanged(false);
    }
}

void PasswordKeeper::setMasterPassword(QByteArray masterPassword, Done done)
{
    if (isSetUp()) {
        PasswordVault::wipe(masterPassword);
        done(false); // change it instead
        return;
    }
    runOffThread<PasswordVault::Stored>(
        this,
        [password = std::move(masterPassword), cost = m_cost]() mutable {
            PasswordVault::Stored stored = PasswordVault::create(password, cost);
            PasswordVault::wipe(password);
            return stored;
        },
        [this, done = std::move(done)](PasswordVault::Stored stored) {
            try {
                m_repo.saveVault(stored);
            } catch (const db::DbError& e) {
                qCWarning(lcPasswords) << "could not save the password vault:" << e.what();
                done(false);
                return;
            }
            m_vault.load(stored);
            m_vault.setMatchKey(matchKey(true));
            emit vaultChanged();
            done(true);
        });
}

void PasswordKeeper::changeMasterPassword(QByteArray oldPassword, QByteArray newPassword, Done done)
{
    if (!m_vault.stored()) {
        PasswordVault::wipe(oldPassword);
        PasswordVault::wipe(newPassword);
        done(false);
        return;
    }
    const PasswordVault::Stored current = *m_vault.stored();
    runOffThread<std::optional<PasswordVault::Rekeyed>>(
        this,
        [current, oldPassword = std::move(oldPassword), newPassword = std::move(newPassword), cost = m_cost]() mutable {
            auto rekeyed = PasswordVault::changePassword(current, oldPassword, newPassword, cost);
            PasswordVault::wipe(oldPassword);
            PasswordVault::wipe(newPassword);
            return rekeyed;
        },
        [this, current, done = std::move(done)](std::optional<PasswordVault::Rekeyed> rekeyed) {
            if (!rekeyed) {
                done(false);
                return;
            }
            bool ok = true;
            try {
                // Every password moves to the new key pair, or none does.
                std::vector<std::pair<qint64, QByteArray>> resealed;
                for (const auto& [id, sealed] : m_repo.allSealedSecrets()) {
                    std::optional<QByteArray> moved = PasswordVault::reseal(
                        sealed, rekeyed->oldSecret, current.publicKey, rekeyed->stored.publicKey);
                    if (!moved) {
                        ok = false;
                        break;
                    }
                    resealed.emplace_back(id, *moved);
                }
                if (ok) {
                    m_repo.rekeyVault(resealed, rekeyed->stored);
                }
            } catch (const db::DbError& e) {
                qCWarning(lcPasswords) << "could not re-seal the password copies:" << e.what();
                ok = false;
            }
            PasswordVault::wipe(rekeyed->oldSecret);
            if (!ok) {
                done(false);
                return;
            }
            const QByteArray match = m_vault.canMatch() ? matchKey(false) : QByteArray();
            lock();
            m_vault.load(rekeyed->stored);
            m_vault.setMatchKey(match);
            emit vaultChanged();
            done(true);
        });
}

void PasswordKeeper::reset()
{
    lock();
    try {
        m_repo.resetVault();
    } catch (const db::DbError& e) {
        qCWarning(lcPasswords) << "could not remove the passwords:" << e.what();
    }
    if (m_matchKeys) {
        m_matchKeys->remove(nullptr);
    }
    m_vault.load(std::nullopt);
    m_vault.setMatchKey({});
    emit vaultChanged();
}

void PasswordKeeper::setHelloWrapped(const QByteArray& wrapped)
{
    if (!m_vault.stored()) {
        return;
    }
    PasswordVault::Stored stored = *m_vault.stored();
    stored.helloWrapped = wrapped;
    try {
        m_repo.saveVault(stored);
    } catch (const db::DbError& e) {
        qCWarning(lcPasswords) << "could not save the password vault:" << e.what();
        return;
    }
    m_vault.replaceStored(stored);
    emit vaultChanged();
}

bool PasswordKeeper::unlockWithHelloKey(const QByteArray& key)
{
    if (!helloEnabled() || !m_vault.unlockWithKey(key, /*hello=*/true)) {
        return false;
    }
    startLockTimer();
    emit lockChanged(true);
    return true;
}

std::optional<QByteArray> PasswordKeeper::reveal(qint64 id) const
{
    if (!isUnlocked()) {
        return std::nullopt;
    }
    try {
        const QByteArray sealed = m_repo.sealedSecret(id);
        return sealed.isEmpty() ? std::nullopt : m_vault.open(sealed);
    } catch (const db::DbError& e) {
        qCWarning(lcPasswords) << "could not read a password copy:" << e.what();
        return std::nullopt;
    }
}

std::optional<qint64> PasswordKeeper::unmark(qint64 id)
{
    std::optional<QByteArray> plain = reveal(id);
    if (!plain) {
        return std::nullopt;
    }
    QString text = QString::fromUtf8(*plain);
    PasswordVault::wipe(*plain);
    std::optional<qint64> holder;
    try {
        holder = m_repo.convertToText(id, text);
    } catch (const db::DbError& e) {
        qCWarning(lcPasswords) << "could not turn a password copy into text:" << e.what();
    }
    PasswordVault::wipe(text);
    return holder;
}

std::optional<qint64> PasswordKeeper::markAsPassword(qint64 id)
{
    if (!isSetUp()) {
        return std::nullopt;
    }
    try {
        const auto entry = m_repo.get(id);
        if (!entry || entry->secret || entry->image) {
            return std::nullopt;
        }
        QByteArray plain = entry->content.toUtf8();
        const QByteArray sealed = m_vault.seal(plain);
        const QString match = m_vault.match(plain);
        PasswordVault::wipe(plain);
        return m_repo.convertToSecret(id, sealed, match);
    } catch (const db::DbError& e) {
        qCWarning(lcPasswords) << "could not mark a copy as a password:" << e.what();
        return std::nullopt;
    }
}

std::vector<qint64> PasswordKeeper::search(const QString& typed) const
{
    std::vector<qint64> found;
    if (typed.isEmpty() || !isSetUp()) {
        return found;
    }
    try {
        if (isUnlocked()) {
            for (auto& [id, sealed] : m_repo.allSealedSecrets()) {
                std::optional<QByteArray> plain = m_vault.open(sealed);
                if (!plain) {
                    continue;
                }
                QString text = QString::fromUtf8(*plain);
                PasswordVault::wipe(*plain);
                if (text.contains(typed, Qt::CaseInsensitive)) {
                    found.push_back(id);
                }
                PasswordVault::wipe(text);
            }
        } else {
            QByteArray plain = typed.toUtf8();
            const QString match = m_vault.match(plain);
            PasswordVault::wipe(plain);
            if (const auto entry = m_repo.findSecret(match)) {
                found.push_back(entry->id);
            }
        }
    } catch (const db::DbError& e) {
        qCWarning(lcPasswords) << "password search failed:" << e.what();
    }
    return found;
}

} // namespace clipitto
