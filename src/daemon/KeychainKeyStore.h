#pragma once

#include "KeyStore.h"

#include <utility>

namespace clipitto {

// A key in the system keyring through qtkeychain: Secret Service (GNOME Keyring,
// KWallet) on Linux, Credential Manager on Windows. Each call runs a keychain job and waits
// for it in a local event loop (bounded), so the caller gets a plain result.
class KeychainKeyStore : public KeyStore {
public:
    explicit KeychainKeyStore(QString entry) : m_entry(std::move(entry)) {}

    bool available(QString* why = nullptr) override;
    std::optional<QByteArray> read(QString* error = nullptr) override;
    bool write(const QByteArray& key, QString* error = nullptr) override;
    bool remove(QString* error = nullptr) override;

private:
    QString m_entry; // the keyring entry's name under the ClipItTo service
};

} // namespace clipitto
