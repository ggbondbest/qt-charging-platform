#pragma once

#include "charging/common/model/models.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <optional>

namespace charging::server {

struct ReservationListItem
{
    charging::model::Reservation reservation;
    QString orderNo;
    QString stationName;
    QString chargerCode;
    charging::model::ChargerType chargerType = charging::model::ChargerType::Slow;
    int chargerPowerWatts = 0;
    qint64 priceCentsPerKwh = 0;
};

struct ReservationQueryResult
{
    bool ok = false;
    QVector<ReservationListItem> reservations;
    int totalCount = 0;
    QString errorMessage;
};

class ReservationQueryRepository final
{
public:
    explicit ReservationQueryRepository(const QSqlDatabase& database);

    ReservationQueryResult listByUser(
        qint64 userId, std::optional<charging::model::ReservationStatus> status = std::nullopt,
        int limit = 20, int offset = 0) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
