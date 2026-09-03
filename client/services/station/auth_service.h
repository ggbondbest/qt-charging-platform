#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"

#include <QObject>
#include <QString>

namespace charging::client::network {
class ClientConnection;
}

namespace charging::client::services::station {

class AuthService final : public QObject
{
    Q_OBJECT

public:
    explicit AuthService(network::ClientConnection* connection, QObject* parent = nullptr);

    void login(const QString& phone);
    bool isLoginPending() const;

signals:
    void loginStarted();
    void loginSucceeded(const charging::model::User& user, bool created);
    void loginFailed(const QString& message);

private:
    void handleResponse(const charging::protocol::ResponseEnvelope& response);
    void handleRequestFailure(const QString& requestId, const QString& errorCode,
                              const QString& message);

    network::ClientConnection* connection_ = nullptr;
    QString pendingLoginRequestId_;
};

} // namespace charging::client::services::station
