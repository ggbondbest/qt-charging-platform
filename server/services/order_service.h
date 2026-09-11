#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"
#include "service_clock.h"

namespace charging::server {

class OrderRepository;

struct PaymentResult
{
    bool success = false;
    bool idempotent = false;
    charging::model::Order order;
    qint64 balanceCents = 0;
    charging::protocol::ProtocolError error;
};

class OrderService final
{
public:
    explicit OrderService(OrderRepository* orderRepository, UtcClock clock = {});

    PaymentResult pay(qint64 userId, qint64 orderId) const;

private:
    OrderRepository* orderRepository_ = nullptr;
    UtcClock clock_;
};

} // namespace charging::server
