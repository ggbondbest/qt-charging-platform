#pragma once

#include "charging/common/model/enums.h"
#include "charging/common/model/models.h"

#include <QDateTime>
#include <QSqlQuery>
#include <QVariant>

namespace charging::server::repository_detail {

inline bool readRequiredUtc(const QVariant& stored, QDateTime* value)
{
    QDateTime parsed = QDateTime::fromString(stored.toString(), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(stored.toString(), Qt::ISODate);
    }
    if (!parsed.isValid()) {
        return false;
    }
    *value = parsed.toUTC();
    return true;
}

inline bool readOptionalUtc(const QVariant& stored, QDateTime* value)
{
    if (stored.isNull() || stored.toString().isEmpty()) {
        *value = QDateTime();
        return true;
    }
    return readRequiredUtc(stored, value);
}

inline bool inSafeRange(qint64 value)
{
    return value >= 0 && value <= charging::model::kMaximumJsonSafeInteger;
}

// Expected columns: id, user_id, charger_id, status, reserved_at,
// expires_at, ended_at.
inline bool readReservation(const QSqlQuery& query, charging::model::Reservation* reservation)
{
    bool idOk = false;
    bool userIdOk = false;
    bool chargerIdOk = false;
    charging::model::Reservation value;
    value.id = query.value(0).toLongLong(&idOk);
    value.userId = query.value(1).toLongLong(&userIdOk);
    value.chargerId = query.value(2).toLongLong(&chargerIdOk);
    if (!idOk || !userIdOk || !chargerIdOk || value.id <= 0 || value.userId <= 0 ||
        value.chargerId <= 0 ||
        !charging::model::fromString(query.value(3).toString(), &value.status) ||
        !readRequiredUtc(query.value(4), &value.reservedAtUtc) ||
        !readRequiredUtc(query.value(5), &value.expiresAtUtc) ||
        !readOptionalUtc(query.value(6), &value.endedAtUtc)) {
        return false;
    }
    *reservation = value;
    return true;
}

// Expected columns: id, order_no, user_id, charger_id, reservation_id,
// status, unit_price, energy, duration, amount, created, started, stopped,
// paid, updated.
inline bool readOrder(const QSqlQuery& query, charging::model::Order* order)
{
    bool idOk = false;
    bool userIdOk = false;
    bool chargerIdOk = false;
    bool reservationIdOk = true;
    bool priceOk = false;
    bool energyOk = false;
    bool durationOk = false;
    bool amountOk = false;

    charging::model::Order value;
    value.id = query.value(0).toLongLong(&idOk);
    value.orderNo = query.value(1).toString();
    value.userId = query.value(2).toLongLong(&userIdOk);
    value.chargerId = query.value(3).toLongLong(&chargerIdOk);
    if (!query.value(4).isNull()) {
        value.reservationId = query.value(4).toLongLong(&reservationIdOk);
    }
    value.unitPriceCentsPerKwh = query.value(6).toLongLong(&priceOk);
    value.energyWh = query.value(7).toLongLong(&energyOk);
    value.durationSeconds = query.value(8).toLongLong(&durationOk);
    value.amountCents = query.value(9).toLongLong(&amountOk);

    if (!idOk || !userIdOk || !chargerIdOk || !reservationIdOk || !priceOk || !energyOk ||
        !durationOk || !amountOk || value.id <= 0 || value.userId <= 0 || value.chargerId <= 0 ||
        value.reservationId < 0 || value.orderNo.isEmpty() ||
        !inSafeRange(value.unitPriceCentsPerKwh) || !inSafeRange(value.energyWh) ||
        !inSafeRange(value.durationSeconds) || !inSafeRange(value.amountCents) ||
        !charging::model::fromString(query.value(5).toString(), &value.status) ||
        !readRequiredUtc(query.value(10), &value.createdAtUtc) ||
        !readOptionalUtc(query.value(11), &value.startedAtUtc) ||
        !readOptionalUtc(query.value(12), &value.stoppedAtUtc) ||
        !readOptionalUtc(query.value(13), &value.paidAtUtc) ||
        !readRequiredUtc(query.value(14), &value.updatedAtUtc)) {
        return false;
    }
    *order = value;
    return true;
}

// Expected columns: id, station_id, code, type, power_watts, status,
// total_charge_count, total_charge_seconds, created_at, updated_at.
inline bool readCharger(const QSqlQuery& query, charging::model::Charger* charger)
{
    bool idOk = false;
    bool stationIdOk = false;
    bool powerOk = false;
    bool countOk = false;
    bool secondsOk = false;

    charging::model::Charger value;
    value.id = query.value(0).toLongLong(&idOk);
    value.stationId = query.value(1).toLongLong(&stationIdOk);
    value.code = query.value(2).toString();
    value.powerWatts = query.value(4).toInt(&powerOk);
    value.totalChargeCount = query.value(6).toInt(&countOk);
    value.totalChargeSeconds = query.value(7).toLongLong(&secondsOk);
    if (!idOk || !stationIdOk || !powerOk || !countOk || !secondsOk || value.id <= 0 ||
        value.stationId <= 0 || value.code.isEmpty() || value.powerWatts <= 0 ||
        value.totalChargeCount < 0 || !inSafeRange(value.totalChargeSeconds) ||
        !charging::model::fromString(query.value(3).toString(), &value.type) ||
        !charging::model::fromString(query.value(5).toString(), &value.status) ||
        !readRequiredUtc(query.value(8), &value.createdAtUtc) ||
        !readRequiredUtc(query.value(9), &value.updatedAtUtc)) {
        return false;
    }
    *charger = value;
    return true;
}

} // namespace charging::server::repository_detail
