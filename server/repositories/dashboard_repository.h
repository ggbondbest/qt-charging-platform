#pragma once

#include <QDate>
#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace charging::server {

struct DashboardSummary
{
    int totalUsers = 0;
    int activeStations = 0;
    int totalChargers = 0;
    int availableChargers = 0;
    int reservedChargers = 0;
    int chargingChargers = 0;
    int faultChargers = 0;
    int offlineChargers = 0;
    int activeOrders = 0;
    qint64 todayRevenueCents = 0;
    qint64 monthRevenueCents = 0;
};

struct DashboardSummaryResult
{
    bool ok = false;
    DashboardSummary summary;
    QString errorMessage;
};

struct RevenuePoint
{
    QDate date;
    int completedOrderCount = 0;
    qint64 revenueCents = 0;
};

struct RevenueTrendResult
{
    bool ok = false;
    QVector<RevenuePoint> points;
    QString errorMessage;
};

class DashboardRepository final
{
public:
    explicit DashboardRepository(const QSqlDatabase& database);

    DashboardSummaryResult summary(const QDateTime& observedAtUtc) const;
    RevenueTrendResult revenueTrend(const QDate& fromDate, const QDate& toDate) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
