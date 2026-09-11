#include "dashboard_repository.h"

#include "charging/common/model/models.h"

#include <QHash>
#include <QSqlError>
#include <QSqlQuery>

namespace charging::server {
namespace {

QString startOfDayUtc(const QDate& date)
{
    return QDateTime(date, QTime(0, 0), Qt::UTC).toString(Qt::ISODateWithMs);
}

bool readNonNegativeInteger(const QSqlQuery& query, int column, int* value)
{
    bool ok = false;
    const int parsed = query.value(column).toInt(&ok);
    if (!ok || parsed < 0) {
        return false;
    }
    *value = parsed;
    return true;
}

bool readMoney(const QSqlQuery& query, int column, qint64* value)
{
    bool ok = false;
    const qint64 parsed = query.value(column).toLongLong(&ok);
    if (!ok || parsed < 0 || parsed > charging::model::kMaximumJsonSafeInteger) {
        return false;
    }
    *value = parsed;
    return true;
}

} // namespace

DashboardRepository::DashboardRepository(const QSqlDatabase& database) : database_(database) {}

DashboardSummaryResult DashboardRepository::summary(const QDateTime& observedAtUtc) const
{
    DashboardSummaryResult result;
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }
    if (!observedAtUtc.isValid()) {
        result.errorMessage = QStringLiteral("The observation time is invalid");
        return result;
    }

    const QDate observedDate = observedAtUtc.toUTC().date();
    const QDate monthStart(observedDate.year(), observedDate.month(), 1);
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT "
        "(SELECT COUNT(*) FROM users), "
        "(SELECT COUNT(*) FROM stations WHERE status = 'ACTIVE'), "
        "COUNT(*), "
        "COALESCE(SUM(CASE WHEN status = 'AVAILABLE' THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN status = 'RESERVED' THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN status = 'CHARGING' THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN status = 'FAULT' THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN status = 'OFFLINE' THEN 1 ELSE 0 END), 0), "
        "(SELECT COUNT(*) FROM orders WHERE status IN "
        "('RESERVED', 'CHARGING', 'WAITING_PAYMENT')), "
        "(SELECT COALESCE(SUM(amount_cents), 0) FROM orders "
        "WHERE status = 'COMPLETED' AND paid_at >= :todayStart AND paid_at < :tomorrowStart), "
        "(SELECT COALESCE(SUM(amount_cents), 0) FROM orders "
        "WHERE status = 'COMPLETED' AND paid_at >= :monthStart AND paid_at < :tomorrowStart) "
        "FROM chargers"));
    query.bindValue(QStringLiteral(":todayStart"), startOfDayUtc(observedDate));
    query.bindValue(QStringLiteral(":tomorrowStart"), startOfDayUtc(observedDate.addDays(1)));
    query.bindValue(QStringLiteral(":monthStart"), startOfDayUtc(monthStart));
    if (!query.exec() || !query.next()) {
        result.errorMessage = query.lastError().text();
        return result;
    }

    DashboardSummary& value = result.summary;
    if (!readNonNegativeInteger(query, 0, &value.totalUsers) ||
        !readNonNegativeInteger(query, 1, &value.activeStations) ||
        !readNonNegativeInteger(query, 2, &value.totalChargers) ||
        !readNonNegativeInteger(query, 3, &value.availableChargers) ||
        !readNonNegativeInteger(query, 4, &value.reservedChargers) ||
        !readNonNegativeInteger(query, 5, &value.chargingChargers) ||
        !readNonNegativeInteger(query, 6, &value.faultChargers) ||
        !readNonNegativeInteger(query, 7, &value.offlineChargers) ||
        !readNonNegativeInteger(query, 8, &value.activeOrders) ||
        !readMoney(query, 9, &value.todayRevenueCents) ||
        !readMoney(query, 10, &value.monthRevenueCents)) {
        result.errorMessage = QStringLiteral("The dashboard aggregate contains invalid data");
        return result;
    }
    result.ok = true;
    return result;
}

RevenueTrendResult DashboardRepository::revenueTrend(const QDate& fromDate,
                                                      const QDate& toDate) const
{
    RevenueTrendResult result;
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }
    const qint64 dayCount = fromDate.daysTo(toDate) + 1;
    if (!fromDate.isValid() || !toDate.isValid() || dayCount <= 0 || dayCount > 31) {
        result.errorMessage = QStringLiteral("The revenue trend range must be 1 to 31 days");
        return result;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT substr(paid_at, 1, 10), COUNT(*), COALESCE(SUM(amount_cents), 0) "
        "FROM orders WHERE status = 'COMPLETED' AND paid_at >= :fromDate "
        "AND paid_at < :toDateExclusive GROUP BY substr(paid_at, 1, 10)"));
    query.bindValue(QStringLiteral(":fromDate"), startOfDayUtc(fromDate));
    query.bindValue(QStringLiteral(":toDateExclusive"), startOfDayUtc(toDate.addDays(1)));
    if (!query.exec()) {
        result.errorMessage = query.lastError().text();
        return result;
    }

    QHash<QDate, RevenuePoint> storedPoints;
    while (query.next()) {
        RevenuePoint point;
        point.date = QDate::fromString(query.value(0).toString(), Qt::ISODate);
        if (!point.date.isValid() || !readNonNegativeInteger(query, 1, &point.completedOrderCount) ||
            !readMoney(query, 2, &point.revenueCents)) {
            result.errorMessage = QStringLiteral("The revenue trend row contains invalid data");
            return result;
        }
        storedPoints.insert(point.date, point);
    }
    for (QDate date = fromDate; date <= toDate; date = date.addDays(1)) {
        result.points.append(storedPoints.value(date, RevenuePoint{date, 0, 0}));
    }
    result.ok = true;
    return result;
}

} // namespace charging::server
