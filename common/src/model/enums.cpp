#include "charging/common/model/enums.h"

namespace charging::model {

namespace {

template <typename Enum>
bool assignIfEqual(const QString& text, const char* expected, Enum value, Enum* outValue)
{
    if (outValue == nullptr || text != QString::fromLatin1(expected)) {
        return false;
    }
    *outValue = value;
    return true;
}

} // namespace

QString toString(UserStatus value)
{
    switch (value) {
    case UserStatus::Active:
        return QStringLiteral("ACTIVE");
    case UserStatus::Frozen:
        return QStringLiteral("FROZEN");
    }
    return {};
}

QString toString(AdminStatus value)
{
    switch (value) {
    case AdminStatus::Active:
        return QStringLiteral("ACTIVE");
    case AdminStatus::Disabled:
        return QStringLiteral("DISABLED");
    }
    return {};
}

QString toString(StationStatus value)
{
    switch (value) {
    case StationStatus::Active:
        return QStringLiteral("ACTIVE");
    case StationStatus::Inactive:
        return QStringLiteral("INACTIVE");
    }
    return {};
}

QString toString(ChargerType value)
{
    switch (value) {
    case ChargerType::Fast:
        return QStringLiteral("FAST");
    case ChargerType::Slow:
        return QStringLiteral("SLOW");
    }
    return {};
}

QString toString(ChargerStatus value)
{
    switch (value) {
    case ChargerStatus::Available:
        return QStringLiteral("AVAILABLE");
    case ChargerStatus::Reserved:
        return QStringLiteral("RESERVED");
    case ChargerStatus::Charging:
        return QStringLiteral("CHARGING");
    case ChargerStatus::Fault:
        return QStringLiteral("FAULT");
    case ChargerStatus::Offline:
        return QStringLiteral("OFFLINE");
    }
    return {};
}

QString toString(ReservationStatus value)
{
    switch (value) {
    case ReservationStatus::Active:
        return QStringLiteral("ACTIVE");
    case ReservationStatus::Fulfilled:
        return QStringLiteral("FULFILLED");
    case ReservationStatus::Cancelled:
        return QStringLiteral("CANCELLED");
    case ReservationStatus::Expired:
        return QStringLiteral("EXPIRED");
    }
    return {};
}

QString toString(OrderStatus value)
{
    switch (value) {
    case OrderStatus::Reserved:
        return QStringLiteral("RESERVED");
    case OrderStatus::Charging:
        return QStringLiteral("CHARGING");
    case OrderStatus::WaitingPayment:
        return QStringLiteral("WAITING_PAYMENT");
    case OrderStatus::Completed:
        return QStringLiteral("COMPLETED");
    case OrderStatus::Cancelled:
        return QStringLiteral("CANCELLED");
    }
    return {};
}

QString toString(RechargeStatus value)
{
    switch (value) {
    case RechargeStatus::Success:
        return QStringLiteral("SUCCESS");
    case RechargeStatus::Failed:
        return QStringLiteral("FAILED");
    }
    return {};
}

bool fromString(const QString& text, UserStatus* outValue)
{
    return assignIfEqual(text, "ACTIVE", UserStatus::Active, outValue) ||
           assignIfEqual(text, "FROZEN", UserStatus::Frozen, outValue);
}

bool fromString(const QString& text, AdminStatus* outValue)
{
    return assignIfEqual(text, "ACTIVE", AdminStatus::Active, outValue) ||
           assignIfEqual(text, "DISABLED", AdminStatus::Disabled, outValue);
}

bool fromString(const QString& text, StationStatus* outValue)
{
    return assignIfEqual(text, "ACTIVE", StationStatus::Active, outValue) ||
           assignIfEqual(text, "INACTIVE", StationStatus::Inactive, outValue);
}

bool fromString(const QString& text, ChargerType* outValue)
{
    return assignIfEqual(text, "FAST", ChargerType::Fast, outValue) ||
           assignIfEqual(text, "SLOW", ChargerType::Slow, outValue);
}

bool fromString(const QString& text, ChargerStatus* outValue)
{
    return assignIfEqual(text, "AVAILABLE", ChargerStatus::Available, outValue) ||
           assignIfEqual(text, "RESERVED", ChargerStatus::Reserved, outValue) ||
           assignIfEqual(text, "CHARGING", ChargerStatus::Charging, outValue) ||
           assignIfEqual(text, "FAULT", ChargerStatus::Fault, outValue) ||
           assignIfEqual(text, "OFFLINE", ChargerStatus::Offline, outValue);
}

bool fromString(const QString& text, ReservationStatus* outValue)
{
    return assignIfEqual(text, "ACTIVE", ReservationStatus::Active, outValue) ||
           assignIfEqual(text, "FULFILLED", ReservationStatus::Fulfilled, outValue) ||
           assignIfEqual(text, "CANCELLED", ReservationStatus::Cancelled, outValue) ||
           assignIfEqual(text, "EXPIRED", ReservationStatus::Expired, outValue);
}

bool fromString(const QString& text, OrderStatus* outValue)
{
    return assignIfEqual(text, "RESERVED", OrderStatus::Reserved, outValue) ||
           assignIfEqual(text, "CHARGING", OrderStatus::Charging, outValue) ||
           assignIfEqual(text, "WAITING_PAYMENT", OrderStatus::WaitingPayment, outValue) ||
           assignIfEqual(text, "COMPLETED", OrderStatus::Completed, outValue) ||
           assignIfEqual(text, "CANCELLED", OrderStatus::Cancelled, outValue);
}

bool fromString(const QString& text, RechargeStatus* outValue)
{
    return assignIfEqual(text, "SUCCESS", RechargeStatus::Success, outValue) ||
           assignIfEqual(text, "FAILED", RechargeStatus::Failed, outValue);
}

} // namespace charging::model
