#pragma once

#include "charging/common/model/enums.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace charging::model {

// qint64 values represented as JSON numbers must stay in the exact IEEE-754
// integer range. IDs are exempt because the wire format represents them as
// decimal strings.
inline constexpr qint64 kMaximumJsonSafeInteger = 9007199254740991LL;
inline constexpr qint64 kMinimumJsonSafeInteger = -kMaximumJsonSafeInteger;

// Zero is the in-memory "not persisted" ID. JSON uses decimal strings for IDs
// so large SQLite integers never pass through an IEEE-754 number.
struct User
{
    qint64 id = 0;
    QString phone;
    QString nickname;
    QString avatarKey;
    qint64 balanceCents = 0;
    UserStatus status = UserStatus::Active;
    QDateTime createdAtUtc;
    QDateTime updatedAtUtc;
};

struct Admin
{
    qint64 id = 0;
    QString username;
    QString displayName;
    AdminStatus status = AdminStatus::Active;
    QDateTime createdAtUtc;
};

struct Station
{
    qint64 id = 0;
    QString code;
    QString name;
    QString address;
    double latitude = 0.0;
    double longitude = 0.0;
    qint64 priceCentsPerKwh = 0;
    StationStatus status = StationStatus::Active;
    int totalChargers = 0;     // Derived, not persisted.
    int availableChargers = 0; // Derived, not persisted.
};

struct Charger
{
    qint64 id = 0;
    qint64 stationId = 0;
    QString code;
    ChargerType type = ChargerType::Slow;
    int powerWatts = 0;
    ChargerStatus status = ChargerStatus::Available;
    int totalChargeCount = 0;
    qint64 totalChargeSeconds = 0;
    QDateTime createdAtUtc;
    QDateTime updatedAtUtc;
};

struct Reservation
{
    qint64 id = 0;
    qint64 userId = 0;
    qint64 chargerId = 0;
    ReservationStatus status = ReservationStatus::Active;
    QDateTime reservedAtUtc;
    QDateTime expiresAtUtc;
    QDateTime endedAtUtc;
};

struct Order
{
    qint64 id = 0;
    QString orderNo;
    qint64 userId = 0;
    qint64 chargerId = 0;
    qint64 reservationId = 0;
    OrderStatus status = OrderStatus::Reserved;
    qint64 unitPriceCentsPerKwh = 0;
    qint64 energyWh = 0;
    qint64 durationSeconds = 0;
    qint64 amountCents = 0;
    QDateTime createdAtUtc;
    QDateTime startedAtUtc;
    QDateTime stoppedAtUtc;
    QDateTime paidAtUtc;
    QDateTime updatedAtUtc;
};

struct RechargeRecord
{
    qint64 id = 0;
    QString transactionNo;
    qint64 userId = 0;
    qint64 amountCents = 0;
    qint64 balanceAfterCents = 0;
    RechargeStatus status = RechargeStatus::Success;
    QDateTime createdAtUtc;
};

struct OperationLog
{
    qint64 id = 0;
    qint64 adminId = 0;
    QString action;
    QString targetType;
    QString targetId;
    QJsonObject details;
    QDateTime createdAtUtc;
};

} // namespace charging::model
