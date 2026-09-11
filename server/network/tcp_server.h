#pragma once

#include "charging_server.h"

namespace charging::server {

// Compatibility entry point for the project role name. ChargingServer remains
// the implementation so the established server startup API stays unchanged.
using TcpServer = ChargingServer;

} // namespace charging::server
