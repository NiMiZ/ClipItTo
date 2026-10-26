#include "SystemKeyStore.h"

#include "KeychainKeyStore.h"

namespace clipitto {

std::shared_ptr<KeyStore> createSystemKeyStore(const QString& entry)
{
    return std::make_shared<KeychainKeyStore>(entry);
}

} // namespace clipitto
