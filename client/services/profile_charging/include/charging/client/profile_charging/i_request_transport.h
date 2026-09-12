#pragma once

#include "charging/common/protocol/protocol.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <functional>

namespace charging::client {

// Abstraction over the client transport used by profile/charging services.
//
// NetworkRequestTransport adapts ClientConnection (requestId correlation and
// timeout); reconnect requires re-login. MockRequestTransport remains for previews.
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

    // Empty for previews/mocks. Live transports scope persisted recharge
    // intents by server endpoint and authenticated account.
    virtual QString persistenceScope() const { return {}; }

    void sendFor(QObject* receiver, const QString& type, const QJsonObject& data,
                 const ResponseCallback& callback)
    {
        const QPointer<QObject> guard(receiver);
        send(type, data, [guard, callback](bool ok, const QJsonObject& result,
                                         const charging::protocol::ProtocolError& error) {
            if (guard && callback) callback(ok, result, error);
        });
    }

    // Sends one v1-style request. The callback is invoked asynchronously on
    // the GUI thread exactly once, mirroring the requestId-correlated model.
    virtual void send(const QString& type, const QJsonObject& data,
                      const ResponseCallback& callback) = 0;
};

} // namespace charging::client
