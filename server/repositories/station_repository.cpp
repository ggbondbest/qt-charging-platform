#include "station_repository.h"

#include "charging/common/model/enums.h"

#include <QSqlError>
#include <QSqlQuery>

namespace charging::server {
namespace {

constexpr int kMaximumPageSize = 100;

QString filterClause(const StationQuery& value)
{
    QString clause = QStringLiteral(
        " WHERE (s.name LIKE :keyword COLLATE NOCASE OR "
        "s.address LIKE :keyword COLLATE NOCASE OR s.code LIKE :keyword COLLATE NOCASE)");
    if (value.status.has_value()) {
        clause.append(QStringLiteral(" AND s.status = :status"));
    }
    return clause;
}

void bindFilters(QSqlQuery* query, const StationQuery& value)
{
    query->bindValue(QStringLiteral(":keyword"),
                     QStringLiteral("%%1%").arg(value.keyword.trimmed()));
    if (value.status.has_value()) {
        query->bindValue(QStringLiteral(":status"), charging::model::toString(*value.status));
    }
}

bool readStation(const QSqlQuery& query, charging::model::Station* station)
{
    bool idOk = false;
    bool priceOk = false;
    bool totalOk = false;
    bool availableOk = false;
    charging::model::Station value;
    value.id = query.value(0).toLongLong(&idOk);
    value.code = query.value(1).toString();
    value.name = query.value(2).toString();
    value.address = query.value(3).toString();
    value.latitude = query.value(4).toDouble();
    value.longitude = query.value(5).toDouble();
    value.priceCentsPerKwh = query.value(6).toLongLong(&priceOk);
    value.totalChargers = query.value(8).toInt(&totalOk);
    value.availableChargers = query.value(9).toInt(&availableOk);
    if (!charging::model::fromString(query.value(7).toString(), &value.status) || !idOk ||
        !priceOk || !totalOk || !availableOk || value.id <= 0 || value.code.isEmpty() ||
        value.name.isEmpty() || value.address.isEmpty()) {
        return false;
    }
    *station = value;
    return true;
}

} // namespace

StationRepository::StationRepository(const QSqlDatabase& database) : database_(database) {}

StationQueryResult StationRepository::list(const StationQuery& value) const
{
    StationQueryResult result;
    if (!database_.isOpen()) {
        result.errorMessage = QStringLiteral("Database is not open");
        return result;
    }
    if (value.limit <= 0 || value.limit > kMaximumPageSize || value.offset < 0) {
        result.errorMessage = QStringLiteral("Invalid pagination parameters");
        return result;
    }

    const QString filters = filterClause(value);
    QSqlQuery countQuery(database_);
    countQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM stations s") + filters);
    bindFilters(&countQuery, value);
    if (!countQuery.exec() || !countQuery.next()) {
        result.errorMessage = countQuery.lastError().text();
        return result;
    }
    result.totalCount = countQuery.value(0).toInt();

    QSqlQuery dataQuery(database_);
    dataQuery.prepare(
        QStringLiteral(
            "SELECT s.id, s.code, s.name, s.address, s.latitude, s.longitude, "
            "s.price_cents_per_kwh, s.status, COUNT(c.id), "
            "COALESCE(SUM(CASE WHEN c.status = 'AVAILABLE' THEN 1 ELSE 0 END), 0) "
            "FROM stations s LEFT JOIN chargers c ON c.station_id = s.id")
        + filters
        + QStringLiteral(" GROUP BY s.id ORDER BY s.id ASC LIMIT :limit OFFSET :offset"));
    bindFilters(&dataQuery, value);
    dataQuery.bindValue(QStringLiteral(":limit"), value.limit);
    dataQuery.bindValue(QStringLiteral(":offset"), value.offset);
    if (!dataQuery.exec()) {
        result.errorMessage = dataQuery.lastError().text();
        return result;
    }
    while (dataQuery.next()) {
        charging::model::Station station;
        if (!readStation(dataQuery, &station)) {
            result.errorMessage = QStringLiteral("Invalid station row returned by database");
            result.stations.clear();
            return result;
        }
        result.stations.append(station);
    }
    result.ok = true;
    return result;
}

} // namespace charging::server
