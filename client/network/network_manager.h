#pragma once

#include "network/client_connection.h"

namespace charging::client::network {

// Compatibility entry point for the project role name. ClientConnection
// remains the implementation so existing login code and signal connections do
// not need to change.
using NetworkManager = ClientConnection;

} // namespace charging::client::network
