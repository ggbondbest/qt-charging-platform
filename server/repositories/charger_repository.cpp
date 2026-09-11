#include "charger_repository.h"

#include "charging/common/model/enums.h"
#include "repository_row_mapper.h"

#include <QSqlError>
#include <QSqlQuery>

namespace charging::server {
namespace {

constexpr int kMaximumPageSize = 100;

QString filterClause(const ChargerQuery& value)
{
    QString clause = QStringLiteral(" WHERE station_id = :stationId");
    if (value.status.has_value()) {
        clause.append(QStringLiteral(" AND status = :status"));
    }
    if (value.type.has_value()) {
        clause.append(QStringLiteral(" AND type = :type"));
    }
    return clause;
}

void bindFilters(QSqlQuery* query, const ChargerQuery& value)
{
    query->bindValue(QStringLiteral(":stationId"), value.stationId);
    if (value.status.has_value()) {
        query->bindValue(QStringLiteral(":status"), charging::model::toString(*value.status));
    }
    if (value.type.has_value()) {
        query->bindValue(QStringLiteral(":type"), charging::model::toString(*value.type));
    }
}

} // namespace

ChargerRepository::ChargerRepository(const QSqlDatabase& database) : database_(database) {}

ChargerQueryResult ChargerRepository::listByStation(const ChargerQuery& value) const
{
    ChargerQueryResult result;
    if (!database_.isOpen()) {
        result.errorMessage = QStringLiteral("Database is not open");
        return result;
    }
    if (value.stationId <= 0 || value.limit <= 0 || value.limit > kMaximumPageSize ||
        value.offset < 0) {
        result.errorMessage = QStringLiteral("Invalid charger query parameters");
        return result;
    }

    const QString filters = filterClause(value);
    QSqlQuery countQuery(database_);
    countQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM chargers") + filters);
    bindFilters(&countQuery, value);
    if (!countQuery.exec() || !countQuery.next()) {
        result.errorMessage = countQuery.lastError().text();
        return result;
    }
    result.totalCount = countQuery.value(0).toInt();

    QSqlQuery dataQuery(database_);
    dataQuery.prepare(
        QStringLiteral(
            "SELECT id, station_id, code, type, power_watts, status, total_charge_count, "
            "total_charge_seconds, created_at, updated_at FROM chargers")
        + filters + QStringLiteral(" ORDER BY id ASC LIMIT :limit OFFSET :offset"));
    bindFilters(&dataQuery, value);
    dataQuery.bindValue(QStringLiteral(":limit"), value.limit);
    dataQuery.bindValue(QStringLiteral(":offset"), value.offset);
    if (!dataQuery.exec()) {
        result.errorMessage = dataQuery.lastError().text();
        return result;
    }
    while (dataQuery.next()) {
        charging::model::Charger charger;
        if (!repository_detail::readCharger(dataQuery, &charger)) {
            result.errorMessage = QStringLiteral("Invalid charger row returned by database");
            result.chargers.clear();
            return result;
        }
        result.chargers.append(charger);
    }
    result.ok = true;
    return result;
}

} // namespace charging::server
