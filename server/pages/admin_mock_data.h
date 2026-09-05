#pragma once

#include <charging/common/model/enums.h>

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace charging::server::admin_mock {

// These records are the single deterministic seed for Dashboard summaries and
// their corresponding management pages.  They are presentation-only and must
// be replaced by Service DTOs once the admin contract is frozen.
struct ChargerRecord
{
    QString code;
    QString station;
    QString type;
    QString power;
    QString status;
    int todaySessions = 0;
    int totalSessions = 0;
    QString totalDuration;
    QString lastHeartbeat;
    QString alertType;
    QString alertOccurredAt;
    // Real management DTO identity/version; Mock seeds leave these empty.
    QString serverId{};
    QString expectedUpdatedAt{};
};

struct OrderRecord
{
    QString orderNo;
    QString userName;
    QString phone;
    QString station;
    QString charger;
    QString chargerType;
    charging::model::OrderStatus status = charging::model::OrderStatus::Reserved;
    QString startAt;
    QString duration;
    qint64 energyWh = 0;
    qint64 chargeFeeCents = 0;
    qint64 serviceFeeCents = 0;
    qint64 discountFeeCents = 0;
    QString paymentMethod;
    QString paymentStatus;
    // Real management DTO identity; Mock seeds leave it empty.
    QString serverId{};
};

QVector<ChargerRecord> createChargerRecords();
QVector<OrderRecord> createOrderRecords();

} // namespace charging::server::admin_mock
