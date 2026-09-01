#pragma once

#include <QString>

namespace charging::model {

// Enum ordinals are process-local only. Persist and transmit the uppercase
// strings returned by toString() so enum reordering cannot corrupt data.
enum class UserStatus
{
    Active,
    Frozen
};
enum class AdminStatus
{
    Active,
    Disabled
};
enum class StationStatus
{
    Active,
    Inactive
};
enum class ChargerType
{
    Fast,
    Slow
};
enum class ChargerStatus
{
    Available,
    Reserved,
    Charging,
    Fault,
    Offline
};
enum class ReservationStatus
{
    Active,
    Fulfilled,
    Cancelled,
    Expired
};
enum class OrderStatus
{
    Reserved,
    Charging,
    WaitingPayment,
    Completed,
    Cancelled
};
enum class RechargeStatus
{
    Success,
    Failed
};

QString toString(UserStatus status);
QString toString(AdminStatus status);
QString toString(StationStatus status);
QString toString(ChargerType type);
QString toString(ChargerStatus status);
QString toString(ReservationStatus status);
QString toString(OrderStatus status);
QString toString(RechargeStatus status);

// Parsing is deliberately case-sensitive. On failure, outValue is unchanged.
bool fromString(const QString& text, UserStatus* outValue);
bool fromString(const QString& text, AdminStatus* outValue);
bool fromString(const QString& text, StationStatus* outValue);
bool fromString(const QString& text, ChargerType* outValue);
bool fromString(const QString& text, ChargerStatus* outValue);
bool fromString(const QString& text, ReservationStatus* outValue);
bool fromString(const QString& text, OrderStatus* outValue);
bool fromString(const QString& text, RechargeStatus* outValue);

} // namespace charging::model
