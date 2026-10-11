#pragma once

#include "KeyStore.h"

namespace clipitto {

// The history key in the system keyring through qtkeychain: Secret Service (GNOME Keyring,
// KWallet) on Linux, Credential Manager on Windows. Each call runs a keychain job and waits
// for it in a local event loop (bounded), so the caller gets a plain result.
class KeychainKeyStore : public KeyStore {
public:
    bool available(QString* why = nullptr) override;
    std::optional<QByteArray> read(QString* error = nullptr) override;
    bool write(const QByteArray& key, QString* error = nullptr) override;
    bool remove(QString* error = nullptr) override;
};

} // namespace clipitto
