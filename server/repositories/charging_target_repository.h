#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QVector>
#include <QPair>

namespace charging::server {

bool validateChargingTarget(const QJsonObject& target, QString* diagnostic = nullptr);
// Caller owns the START transaction. An empty target retains manual charging.
bool insertChargingTargetInTransaction(const QSqlDatabase& database, qint64 orderId,
                                      const QJsonObject& target, const QDateTime& createdAt,
                                      QString* diagnostic = nullptr);
bool chargingTargetDto(const QSqlDatabase& database, qint64 orderId, QJsonObject* target,
                       QString* stopReason = nullptr, QString* diagnostic = nullptr);
bool activeChargingTargetOrders(const QSqlDatabase& database,
                                QVector<QPair<qint64, qint64>>* orders,
                                QString* diagnostic = nullptr);

struct TargetMeterSample
{
    bool success = false;
    bool reached = false;
    qint64 durationSeconds = 0;
    qint64 energyWh = 0;
    qint64 amountCents = 0;
    QString stopReason;
};

// All arithmetic is integer based. The final sample may be partial so a
// one-second server tick can never bill past an amount/energy target.
TargetMeterSample calculateTargetSample(int powerWatts, qint64 elapsedSeconds,
                                       qint64 priceCentsPerKwh, const QJsonObject& target);

} // namespace charging::server
