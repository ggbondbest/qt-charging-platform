#pragma once

#include "charging/common/protocol/protocol.h"

#include <QString>

namespace charging::client {

// Single place mapping stable protocol error codes to user-facing text.
// Business logic must branch on the code, never on the message; unknown
// codes fall back to a generic prompt per protocol doc section 8.3.
QString displayMessageForError(const charging::protocol::ProtocolError& error);

} // namespace charging::client
