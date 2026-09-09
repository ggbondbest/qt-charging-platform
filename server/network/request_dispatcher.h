#pragma once

#include "charging/common/protocol/protocol.h"

#include <QtGlobal>
#include <functional>

namespace charging::server {

class ChargingService;
class OrderService;
class UserService;
class UserApiService;
class QueueService;
class RepairService;

class RequestDispatcher final
{
public:
    explicit RequestDispatcher(UserService* userService, ChargingService* chargingService = nullptr,
                               OrderService* orderService = nullptr, UserApiService* userApiService = nullptr);
    void setWorkflowServices(QueueService* queue, RepairService* repair,
                             std::function<bool()> maintenance = {});

    charging::protocol::ResponseEnvelope
    dispatch(const charging::protocol::RequestEnvelope& request,
             qint64* authenticatedUserId = nullptr) const;

private:
    QueueService* queueService_ = nullptr;
    RepairService* repairService_ = nullptr;
    std::function<bool()> maintenance_;
    UserService* userService_ = nullptr;
    UserApiService* userApiService_ = nullptr;
    ChargingService* chargingService_ = nullptr;
    OrderService* orderService_ = nullptr;
};

} // namespace charging::server
