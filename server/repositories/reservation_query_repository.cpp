#include "reservation_query_repository.h"

#include "charging/common/model/enums.h"
#include "repository_row_mapper.h"

#include <QSqlError>
#include <QSqlQuery>

namespace charging::server {
namespace {

constexpr int kMaximumPageSize = 100;

} // namespace

ReservationQueryRepository::ReservationQueryRepository(const QSqlDatabase& database)
    : database_(database)
{
}

ReservationQueryResult ReservationQueryRepository::listByUser(
    qint64 userId, std::optional<charging::model::ReservationStatus> status, int limit,
    int offset) const
{
    ReservationQueryResult result;
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }
    if (userId <= 0 || limit <= 0 || limit > kMaximumPageSize || offset < 0) {
        result.errorMessage = QStringLiteral("Invalid reservation query parameters");
        return result;
    }

    QString filters = QStringLiteral(" WHERE r.user_id = :userId");
    if (status.has_value()) {
        filters.append(QStringLiteral(" AND r.status = :status"));
    }
    const auto bindFilters = [userId, status](QSqlQuery* query) {
        query->bindValue(QStringLiteral(":userId"), userId);
        if (status.has_value()) {
            query->bindValue(QStringLiteral(":status"), charging::model::toString(*status));
        }
    };

    QSqlQuery countQuery(database_);
    countQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM reservations r") + filters);
    bindFilters(&countQuery);
    if (!countQuery.exec() || !countQuery.next()) {
        result.errorMessage = countQuery.lastError().text();
        return result;
    }
    result.totalCount = countQuery.value(0).toInt();

    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral(
            "SELECT r.id, r.user_id, r.charger_id, r.status, r.reserved_at, r.expires_at, "
            "r.ended_at, COALESCE(o.order_no, ''), s.name, c.code, c.type, c.power_watts, "
            "s.price_cents_per_kwh FROM reservations r "
            "JOIN chargers c ON c.id = r.charger_id "
            "JOIN stations s ON s.id = c.station_id "
            "LEFT JOIN orders o ON o.reservation_id = r.id")
        + filters
        + QStringLiteral(" ORDER BY r.reserved_at DESC, r.id DESC LIMIT :limit OFFSET :offset"));
    bindFilters(&query);
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);
    if (!query.exec()) {
        result.errorMessage = query.lastError().text();
        return result;
    }
    while (query.next()) {
        ReservationListItem item;
        bool powerOk = false;
        bool priceOk = false;
        if (!repository_detail::readReservation(query, &item.reservation)) {
            result.errorMessage = QStringLiteral("The stored reservation row contains invalid data");
            result.reservations.clear();
            return result;
        }
        item.orderNo = query.value(7).toString();
        item.stationName = query.value(8).toString();
        item.chargerCode = query.value(9).toString();
        item.chargerPowerWatts = query.value(11).toInt(&powerOk);
        item.priceCentsPerKwh = query.value(12).toLongLong(&priceOk);
        if (!charging::model::fromString(query.value(10).toString(), &item.chargerType) ||
            !powerOk || !priceOk || item.stationName.isEmpty() || item.chargerCode.isEmpty() ||
            item.chargerPowerWatts <= 0 || item.priceCentsPerKwh < 0) {
            result.errorMessage = QStringLiteral("The reservation display data is invalid");
            result.reservations.clear();
            return result;
        }
        result.reservations.append(item);
    }
    result.ok = true;
    return result;
}

} // namespace charging::server
