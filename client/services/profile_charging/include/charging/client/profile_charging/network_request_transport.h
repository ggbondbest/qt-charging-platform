#pragma once
#include "charging/client/profile_charging/i_request_transport.h"
#include "network/client_connection.h"
#include <QHash>
#include <QObject>
#include <QPointer>

namespace charging::client {
// Adapts the already logged-in connection. Never automatically replays writes
// or reuses Session identity after disconnect. Re-login must be explicit.
class NetworkRequestTransport final : public QObject, public IRequestTransport {
    Q_OBJECT
public:
    explicit NetworkRequestTransport(network::ClientConnection* connection,
                                     qint64 authenticatedUserId, QObject* parent = nullptr);
    ~NetworkRequestTransport() override;
    void send(const QString& type, const QJsonObject& data,
              const ResponseCallback& callback) override;
    QString persistenceScope() const override;
private:
    void finish(const QString& id, bool success, const QJsonObject& data,
                const charging::protocol::ProtocolError& error);
    void failAll();
    QPointer<network::ClientConnection> connection_;
    QHash<QString, ResponseCallback> pending_;
    qint64 userId_ = 0;
    bool authenticated_ = false;
};
} // namespace charging::client
