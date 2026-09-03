#pragma once

#include "charging/common/model/enums.h"

namespace charging::server {

class ChargingStateMachine final
{
public:
    static bool canTransition(charging::model::ReservationStatus from,
                              charging::model::ReservationStatus to);
    static bool canTransition(charging::model::OrderStatus from, charging::model::OrderStatus to);
    static bool canTransition(charging::model::ChargerStatus from,
                              charging::model::ChargerStatus to);
};

} // namespace charging::server
