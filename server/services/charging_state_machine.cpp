#include "charging_state_machine.h"

namespace charging::server {

bool ChargingStateMachine::canTransition(charging::model::ReservationStatus from,
                                         charging::model::ReservationStatus to)
{
    using Status = charging::model::ReservationStatus;
    return from == Status::Active &&
           (to == Status::Fulfilled || to == Status::Cancelled || to == Status::Expired);
}

bool ChargingStateMachine::canTransition(charging::model::OrderStatus from,
                                         charging::model::OrderStatus to)
{
    using Status = charging::model::OrderStatus;
    return (from == Status::Reserved && (to == Status::Charging || to == Status::Cancelled)) ||
           (from == Status::Charging && to == Status::WaitingPayment) ||
           (from == Status::WaitingPayment && to == Status::Completed);
}

bool ChargingStateMachine::canTransition(charging::model::ChargerStatus from,
                                         charging::model::ChargerStatus to)
{
    using Status = charging::model::ChargerStatus;
    return (from == Status::Available && to == Status::Reserved) ||
           (from == Status::Reserved && (to == Status::Charging || to == Status::Available)) ||
           (from == Status::Charging && to == Status::Available) ||
           (from == Status::Available && (to == Status::Fault || to == Status::Offline)) ||
           ((from == Status::Fault || from == Status::Offline) && to == Status::Available);
}

} // namespace charging::server
