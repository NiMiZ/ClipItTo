#include "SystemKeyStore.h"

namespace clipitto {

namespace {

// Explicitly unsupported (e.g. a Linux build without qtkeychain): every call says why.
class UnsupportedKeyStore : public KeyStore {
public:
    bool available(QString* why) override { return fail(why); }
    std::optional<QByteArray> read(QString* error) override
    {
        fail(error);
        return std::nullopt;
    }
    bool write(const QByteArray&, QString* error) override { return fail(error); }
    bool remove(QString* error) override { return fail(error); }

private:
    static bool fail(QString* why)
    {
        if (why) {
            *why = QStringLiteral("this build has no system keyring support (install qtkeychain-qt6-dev and rebuild)");
        }
        return false;
    }
};

} // namespace

std::shared_ptr<KeyStore> createSystemKeyStore()
{
    return std::make_shared<UnsupportedKeyStore>();
}

} // namespace clipitto
