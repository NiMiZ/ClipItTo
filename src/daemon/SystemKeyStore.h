#pragma once

#include "KeyStore.h"

#include <memory>

namespace clipitto {

// The platform keyring for the history key, chosen in CMake:
//   Linux:   Secret Service (GNOME Keyring, KWallet) via qtkeychain  - SystemKeyStore_linux.cpp
//   Windows: Credential Manager                                       - SystemKeyStore_win.cpp
//   other / Linux without qtkeychain: unsupported                     - SystemKeyStore_none.cpp
// Never null: an unsupported platform returns a store whose available() says why.
std::shared_ptr<KeyStore> createSystemKeyStore();

} // namespace clipitto
