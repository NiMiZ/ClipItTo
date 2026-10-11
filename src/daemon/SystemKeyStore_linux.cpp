#include "SystemKeyStore.h"

#include "KeychainKeyStore.h"

namespace clipitto {

std::shared_ptr<KeyStore> createSystemKeyStore()
{
    return std::make_shared<KeychainKeyStore>();
}

} // namespace clipitto
