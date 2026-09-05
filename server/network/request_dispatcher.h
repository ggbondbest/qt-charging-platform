#pragma once

#include "charging/common/protocol/protocol.h"

#include <QtGlobal>

namespace charging::server {

class ChargingService;
class OrderService;
class UserService;
class UserApiService;

class RequestDispatcher final
{
public:
    explicit RequestDispatcher(UserService* userService, ChargingService* chargingService = nullptr,
                               OrderService* orderService = nullptr, UserApiService* userApiService = nullptr);

    charging::protocol::ResponseEnvelope
    dispatch(const charging::protocol::RequestEnvelope& request,
             qint64* authenticatedUserId = nullptr) const;

private:
    UserService* userService_ = nullptr;
    UserApiService* userApiService_ = nullptr;
    ChargingService* chargingService_ = nullptr;
    OrderService* orderService_ = nullptr;
};

} // namespace charging::server
