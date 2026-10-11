#include "KeychainKeyStore.h"

#include <QEventLoop>
#include <QTimer>

#include <qt6keychain/keychain.h>

namespace clipitto {

namespace {

const QString kService = QStringLiteral("io.github.nimiz.ClipItTo");
const QString kEntry = QStringLiteral("history-key");
constexpr int kTimeoutMs = 10000; // a locked keyring may show an unlock prompt

// Runs `job` to completion. Returns false (with `error`) on failure or timeout.
bool runJob(QKeychain::Job& job, QString* error, bool notFoundIsError = true)
{
    job.setAutoDelete(false);
    job.setKey(kEntry);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(kTimeoutMs, &loop, &QEventLoop::quit);
    bool finished = false;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, [&finished] { finished = true; });
    job.start();
    if (!finished) {
        loop.exec();
    }
    if (!finished) {
        if (error) {
            *error = QStringLiteral("the system keyring did not answer");
        }
        return false;
    }
    if (job.error() == QKeychain::NoError || (!notFoundIsError && job.error() == QKeychain::EntryNotFound)) {
        return true;
    }
    if (error) {
        *error = job.errorString();
    }
    return false;
}

} // namespace

bool KeychainKeyStore::available(QString* why)
{
    // isAvailable() only says a backend library is present; a lookup proves a keyring
    // service actually answers (on Linux: a Secret Service such as GNOME Keyring).
    QString error;
    if (QKeychain::isAvailable()) {
        QKeychain::ReadPasswordJob probe(kService);
        if (runJob(probe, &error, /*notFoundIsError=*/false)) {
            return true;
        }
    }
    if (why) {
        *why = error.isEmpty() ? QStringLiteral("no system keyring is available") : error;
    }
    return false;
}

std::optional<QByteArray> KeychainKeyStore::read(QString* error)
{
    QKeychain::ReadPasswordJob job(kService);
    if (!runJob(job, error, /*notFoundIsError=*/false)) {
        return std::nullopt;
    }
    if (job.error() == QKeychain::EntryNotFound) {
        if (error) {
            error->clear();
        }
        return std::nullopt;
    }
    return job.binaryData();
}

bool KeychainKeyStore::write(const QByteArray& key, QString* error)
{
    QKeychain::WritePasswordJob job(kService);
    job.setBinaryData(key);
    return runJob(job, error);
}

bool KeychainKeyStore::remove(QString* error)
{
    QKeychain::DeletePasswordJob job(kService);
    return runJob(job, error, /*notFoundIsError=*/false);
}

} // namespace clipitto
