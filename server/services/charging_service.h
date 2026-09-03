#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"
#include "service_clock.h"

namespace charging::server {

class BillingService;
class ChargingRepository;
struct ChargingRepositoryResult;

struct ChargingOperationResult
{
    bool success = false;
    bool idempotent = false;
    charging::model::Reservation reservation;
    charging::model::Order order;
    charging::model::Charger charger;
    // Non-zero only while the returned order is actively charging.
    int currentPowerWatts = 0;
    charging::protocol::ProtocolError error;
};

class ChargingService final
{
public:
    static constexpr qint64 kReservationLifetimeSeconds = 15 * 60;

    ChargingService(ChargingRepository* chargingRepository, BillingService* billingService,
                    UtcClock clock = {});

    ChargingOperationResult reserve(qint64 userId, qint64 chargerId) const;
    ChargingOperationResult cancelReservation(qint64 userId, qint64 reservationId) const;
    ChargingOperationResult startCharging(qint64 userId, qint64 reservationId) const;
    ChargingOperationResult chargingStatus(qint64 userId, qint64 orderId) const;
    ChargingOperationResult stopCharging(qint64 userId, qint64 orderId) const;

private:
    ChargingOperationResult fromRepository(const ChargingRepositoryResult& value) const;

    ChargingRepository* chargingRepository_ = nullptr;
    BillingService* billingService_ = nullptr;
    UtcClock clock_;
};

} // namespace charging::server
