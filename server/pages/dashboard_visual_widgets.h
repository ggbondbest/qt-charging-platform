#pragma once

#include <QColor>
#include <QDate>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QMouseEvent;

namespace charging::server {

class MetricIconWidget final : public QWidget
{
public:
    MetricIconWidget(const QColor& accent, int iconType, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor accent_;
    int iconType_ = 0;
};

class RevenueTrendWidget final : public QWidget
{
public:
    explicit RevenueTrendWidget(QWidget* parent = nullptr);

    void setPeriod(int period);
    void setDisplayMode(int displayMode);
    void setCustomDateRange(const QDate& startDate, const QDate& endDate);
    void setServiceSeries(const QStringList& labels, const QVector<qint64>& revenueCents,
                          const QVector<int>& completedOrders);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    int period_ = 2;
    int displayMode_ = 0;
    QDate customStartDate_;
    QDate customEndDate_;
    int firstVisibleIndex_ = 0;
    int selectedIndex_ = -1;
    int dragStartX_ = 0;
    int dragStartFirstIndex_ = 0;
    bool dragging_ = false;
    bool draggingOverview_ = false;
    QStringList serviceLabels_;
    QVector<qint64> serviceRevenueCents_;
    QVector<int> serviceCompletedOrders_;
};

class DeviceStatusWidget final : public QWidget
{
public:
    explicit DeviceStatusWidget(QWidget* parent = nullptr);
    void setCounts(int online, int offline, int fault);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int online_ = 0;
    int offline_ = 0;
    int fault_ = 0;
};

} // namespace charging::server
