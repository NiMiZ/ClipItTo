#pragma once

#include "LocalProtocol.h"

namespace clipitto::localipc {

// Sends one request to the daemon and waits for the reply. Blocking: meant for the CLI
// and for a second daemon instance forwarding a request; never call it from the thread
// that runs the server.
Reply call(const QString& serverName, const Request& request, int timeoutMs = 5000);

} // namespace clipitto::localipc
