#pragma once
#include "charging/common/protocol/protocol.h"
#include "service_clock.h"

namespace charging::server {
class UserApiRepository;
struct UserApiReply {
    bool success = false;
    QJsonObject data;
    charging::protocol::ProtocolError error;
};
// User-facing query/wallet facade. Keeps protocol mapping out of SQL and avoids
// coupling these endpoints to the separately developed admin query services.
class UserApiService final {
public:
    explicit UserApiService(UserApiRepository* repository, UtcClock clock = {});
    static bool handles(const QString& type);
    UserApiReply handle(const QString& type, const QJsonObject& data, qint64 sessionUserId) const;
private:
    UserApiRepository* repository_;
    UtcClock clock_;
};
} // namespace charging::server
