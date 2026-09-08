#include "delivery_dashboard_widgets.h"

#include <QCategoryAxis>
#include <QChart>
#include <QCursor>
#include <QLegend>
#include <QLineSeries>
#include <QPainter>
#include <QPieSeries>
#include <QPieSlice>
#include <QToolTip>
#include <QValueAxis>

#include <algorithm>

namespace charging::server {

DeliveryRevenueTrendWidget::DeliveryRevenueTrendWidget(QWidget* parent)
    : QChartView(new QChart(), parent)
{
    setObjectName(QStringLiteral("deliveryRevenueChart"));
    setMinimumHeight(240);
    setRenderHint(QPainter::Antialiasing);
    chart()->setAnimationOptions(QChart::NoAnimation);
    chart()->setMargins(QMargins(4, 4, 4, 4));
    chart()->legend()->hide();
    rebuild();
}

void DeliveryRevenueTrendWidget::setPeriod(int period)
{
    if (period < 0 || period > 2)
        return;
    period_ = period;
    rebuild();
}

void DeliveryRevenueTrendWidget::setDisplayMode(int displayMode)
{
    if (displayMode < 0 || displayMode > 1)
        return;
    displayMode_ = displayMode;
    rebuild();
}

void DeliveryRevenueTrendWidget::setCustomDateRange(const QDate& startDate, const QDate& endDate)
{
    if (!startDate.isValid() || !endDate.isValid() || startDate > endDate)
        return;
    customStartDate_ = startDate;
    customEndDate_ = endDate;
    period_ = 3;
    rebuild();
}

void DeliveryRevenueTrendWidget::setServiceSeries(const QStringList& labels,
                                                  const QVector<qint64>& revenueCents,
                                                  const QVector<int>& completedOrders)
{
    labels_.clear();
    revenueCents_.clear();
    completedOrders_.clear();
    bool valid = labels.size() == revenueCents.size() && labels.size() == completedOrders.size();
    QDate previous;
    for (int i = 0; valid && i < labels.size(); ++i) {
        const auto date = QDate::fromString(labels.at(i), Qt::ISODate);
        valid = date.isValid() && (!previous.isValid() || date > previous) &&
                revenueCents.at(i) >= 0 && revenueCents.at(i) <= 9007199254740991LL &&
                completedOrders.at(i) >= 0;
        previous = date;
    }
    if (valid) {
        labels_ = labels;
        revenueCents_ = revenueCents;
        completedOrders_ = completedOrders;
    }
    rebuild();
}

void DeliveryRevenueTrendWidget::rebuild()
{
    chart()->removeAllSeries();
    const auto oldAxes = chart()->axes();
    for (auto* axis : oldAxes) {
        chart()->removeAxis(axis);
        delete axis;
    }
    auto* series = new QLineSeries(chart());
    series->setObjectName(QStringLiteral("deliveryRevenueSeries"));
    series->setName(displayMode_ == 0 ? tr("营收（元）") : tr("完成订单（笔）"));
    series->setPen(QPen(QColor("#347cf6"), 2));
    series->setPointsVisible(true);
    chart()->addSeries(series);
    auto* axisX = new QCategoryAxis(chart());
    axisX->setLabelsPosition(QCategoryAxis::AxisLabelsPositionOnValue);
    axisX->setStartValue(-0.5);
    axisX->setTitleText(tr("日期（北京时间）"));
    auto* axisY = new QValueAxis(chart());
    axisY->setTitleText(displayMode_ == 0 ? tr("营收（元）") : tr("完成订单（笔）"));
    axisY->setLabelFormat(displayMode_ == 0 ? QStringLiteral("%.2f") : QStringLiteral("%.0f"));
    chart()->addAxis(axisX, Qt::AlignBottom);
    chart()->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisX);
    series->attachAxis(axisY);

    const int limit = period_ == 0 ? 1 : period_ == 1 ? 7 : 30;
    const int first = period_ == 3 ? 0 : qMax(0, int(labels_.size()) - limit);
    QStringList shownDates;
    double maximum = 0;
    for (int i = first; i < labels_.size(); ++i) {
        const auto date = QDate::fromString(labels_.at(i), Qt::ISODate);
        if (period_ == 3 && (date < customStartDate_ || date > customEndDate_))
            continue;
        const double value = displayMode_ == 0 ? double(revenueCents_.at(i)) / 100.0
                                               : double(completedOrders_.at(i));
        series->append(shownDates.size(), value);
        shownDates.append(labels_.at(i));
        maximum = qMax(maximum, value);
    }
    const int last = shownDates.size() - 1;
    const int labelStride = qMax(1, (int(shownDates.size()) + 6) / 7);
    for (int i = 0; i < shownDates.size(); ++i) {
        if (i % labelStride == 0 || i == last)
            axisX->append(QDate::fromString(shownDates.at(i), Qt::ISODate).toString("MM-dd"), i);
    }
    axisX->setRange(-0.5, qMax(0.5, double(last) + 0.5));
    axisY->setRange(0, maximum == 0 ? 1.0 : maximum * 1.1);
    // Integer order ticks remain meaningful even when there is only one order.
    if (displayMode_ == 1)
        axisY->setTickCount(qMax(2, qMin(6, int(qMin(maximum + 1, 6.0)))));
    chart()->setTitle(shownDates.isEmpty() ? tr("暂无服务趋势数据") : QString());
    connect(series, &QLineSeries::hovered, this,
            [this, shownDates](const QPointF& point, bool hovering) {
                const int index = qRound(point.x());
                if (!hovering || index < 0 || index >= shownDates.size()) {
                    QToolTip::hideText();
                    return;
                }
                QToolTip::showText(QCursor::pos(),
                                  tr("%1\n%2 %3").arg(shownDates.at(index),
                                      QString::number(point.y(), 'f', displayMode_ == 0 ? 2 : 0),
                                      displayMode_ == 0 ? tr("元") : tr("笔")), this);
            });
}

DeliveryDeviceStatusWidget::DeliveryDeviceStatusWidget(QWidget* parent)
    : QChartView(new QChart(), parent)
{
    setObjectName(QStringLiteral("deliveryDeviceStateChart"));
    setMinimumSize(150, 190);
    setMaximumWidth(210);
    setRenderHint(QPainter::Antialiasing);
    chart()->setAnimationOptions(QChart::NoAnimation);
    chart()->setMargins(QMargins(0, 0, 0, 0));
    chart()->legend()->hide();
    setCounts(0, 0, 0, 0, 0);
}

void DeliveryDeviceStatusWidget::setCounts(int available, int charging, int fault,
                                           int reserved, int offline)
{
    chart()->removeAllSeries();
    const int counts[] = {available, charging, fault, reserved, offline};
    const QStringList names{tr("空闲"), tr("在用"), tr("故障"), tr("预约"), tr("离线")};
    const char* colors[] = {"#43c7bc", "#347cf6", "#f5a130", "#9469d4", "#aab4c2"};
    auto* series = new QPieSeries(chart());
    series->setHoleSize(0.58);
    chart()->addSeries(series);
    qint64 total = 0;
    for (int count : counts) {
        if (count < 0) {
            chart()->setTitle(tr("状态数据无效"));
            return;
        }
        total += count;
    }
    for (int i = 0; i < 5; ++i) {
        if (counts[i] == 0)
            continue;
        auto* slice = series->append(names.at(i), counts[i]);
        slice->setBrush(QColor(colors[i]));
        slice->setPen(QPen(Qt::white, 1));
    }
    chart()->setTitle(total == 0 ? tr("暂无电桩") : QString());
}

} // namespace charging::server
