#pragma once

#include <QChartView>
#include <QDate>
#include <QStringList>
#include <QVector>

class QLineSeries;
class QPieSeries;

namespace charging::server {

// Qt Charts renderer. Values only come from dashboard.get; changing a period
// filters the loaded series and never invents demo revenue or order counts.
class DeliveryRevenueTrendWidget final : public QChartView
{
    Q_OBJECT
public:
    explicit DeliveryRevenueTrendWidget(QWidget* parent = nullptr);
    void setPeriod(int period);
    void setDisplayMode(int displayMode);
    void setCustomDateRange(const QDate& startDate, const QDate& endDate);
    void setServiceSeries(const QStringList& labels, const QVector<qint64>& revenueCents,
                          const QVector<int>& completedOrders);

private:
    void rebuild();
    int period_ = 1;
    int displayMode_ = 0;
    QDate customStartDate_;
    QDate customEndDate_;
    QStringList labels_;
    QVector<qint64> revenueCents_;
    QVector<int> completedOrders_;
};

// Five disjoint operational states. "Online" is deliberately not a slice,
// because it overlaps AVAILABLE, CHARGING, RESERVED and FAULT.
class DeliveryDeviceStatusWidget final : public QChartView
{
    Q_OBJECT
public:
    explicit DeliveryDeviceStatusWidget(QWidget* parent = nullptr);
    void setCounts(int available, int charging, int fault, int reserved, int offline);
};

} // namespace charging::server
