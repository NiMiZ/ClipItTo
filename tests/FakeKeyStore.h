#pragma once

#include "KeyStore.h"

// In-memory stand-in for the system keyring.
class FakeKeyStore : public clipitto::KeyStore {
public:
    bool isAvailable = true;
    bool failWrites = false; // e.g. the keyring refuses or disappears
    bool loseWrites = false; // reports success but keeps nothing (session-only keyring)
    std::optional<QByteArray> stored;

    bool available(QString* why) override
    {
        if (!isAvailable && why) {
            *why = QStringLiteral("no keyring in this test");
        }
        return isAvailable;
    }
    std::optional<QByteArray> read(QString* error) override
    {
        if (error) {
            error->clear();
        }
        return stored;
    }
    bool write(const QByteArray& key, QString* error) override
    {
        if (failWrites) {
            if (error) {
                *error = QStringLiteral("keyring refused");
            }
            return false;
        }
        if (!loseWrites) {
            stored = key;
        }
        return true;
    }
    bool remove(QString*) override
    {
        stored.reset();
        return true;
    }
};
