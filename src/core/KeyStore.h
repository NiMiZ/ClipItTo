#pragma once

#include <QByteArray>
#include <QString>

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
};

} // namespace clipitto
