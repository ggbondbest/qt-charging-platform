#pragma once

#include "charging/common/protocol/protocol.h"

#include <QJsonObject>

#include <functional>

namespace charging::client {

// Abstraction over the client transport used by profile/charging services.
//
// The production implementation will adapt the team-leader owned
// ClientConnection (requestId correlation, timeout, reconnect). During the
// mock phase MockRequestTransport provides deterministic in-memory answers.
// Pages must never see this interface directly; they talk to services only.
class IRequestTransport
{
public:
    // success == true  -> data carries the response payload, error is empty.
    // success == false -> error carries a stable protocol error code.
    using ResponseCallback =
        std::function<void(bool success, const QJsonObject& data,
                           const charging::protocol::ProtocolError& error)>;

    virtual ~IRequestTransport() = default;

    // Sends one v1-style request. The callback is invoked asynchronously on
    // the GUI thread exactly once, mirroring the requestId-correlated model.
    virtual void send(const QString& type, const QJsonObject& data,
                      const ResponseCallback& callback) = 0;
};

} // namespace charging::client
