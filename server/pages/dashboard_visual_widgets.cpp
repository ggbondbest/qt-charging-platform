#include "dashboard_visual_widgets.h"

#include <QLinearGradient>
#include <QList>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QtMath>

#include <algorithm>
#include <utility>

namespace charging::server {

MetricIconWidget::MetricIconWidget(const QColor& accent, int iconType, QWidget* parent)
    : QWidget(parent), accent_(accent), iconType_(iconType)
{
    setFixedSize(54, 54);
}

void MetricIconWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient background(rect().topLeft(), rect().bottomRight());
    background.setColorAt(0, accent_.lighter(112));
    background.setColorAt(1, accent_);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 16, 16);
    painter.setPen(QPen(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const QRectF area = rect().adjusted(15, 15, -15, -15);
    switch (iconType_) {
    case 0: // wallet
        painter.drawRoundedRect(area.adjusted(-2, 2, 2, -1), 3, 3);
        painter.drawLine(area.left() + 4, area.top() + 4, area.right() - 1, area.top() + 4);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(area.right() - 3, area.center().y() + 2), 1.7, 1.7);
        break;
    case 1: // calendar
        painter.drawRoundedRect(area.adjusted(0, 1, 0, 1), 2, 2);
        painter.drawLine(area.left(), area.top() + 7, area.right(), area.top() + 7);
        painter.drawLine(area.left() + 5, area.top() - 1, area.left() + 5, area.top() + 3);
        painter.drawLine(area.right() - 5, area.top() - 1, area.right() - 5, area.top() + 3);
        painter.drawPoint(area.left() + 5, area.top() + 12);
        painter.drawPoint(area.right() - 5, area.top() + 12);
        break;
    case 2: // charging pile
        painter.drawRoundedRect(area.adjusted(4, 0, -4, 0), 2, 2);
        painter.drawLine(area.center().x(), area.top() + 4, area.center().x(), area.bottom() - 4);
        painter.drawLine(area.right() - 3, area.top() + 5, area.right() + 2, area.top() + 5);
        painter.drawLine(area.right() + 2, area.top() + 5, area.right() + 2, area.top() + 11);
        break;
    default: // connected vehicle
        painter.drawRoundedRect(QRectF(area.left(), area.top() + 7, area.width(), 8), 3, 3);
        painter.drawLine(area.left() + 4, area.top() + 7, area.left() + 7, area.top() + 3);
        painter.drawLine(area.right() - 4, area.top() + 7, area.right() - 7, area.top() + 3);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(area.left() + 5, area.bottom() - 1), 2, 2);
        painter.drawEllipse(QPointF(area.right() - 5, area.bottom() - 1), 2, 2);
        break;
    }
}

namespace {

struct TrendSeries {
    QList<qreal> revenueThousands;
    QList<qreal> completedOrderCount;
    QStringList labels;
};

TrendSeries createTrendSeries(int period, const QDate& customStartDate, const QDate& customEndDate)
{
    TrendSeries series;
    const QList<qreal> monthlyRevenue = {132, 112, 91, 105, 131, 161, 169, 160, 143, 150, 165, 170,
                                          153, 157, 185, 193, 177, 152, 155, 161, 132, 126, 130, 104,
                                          89, 111, 125, 119, 102, 109, 154, 172, 176, 157, 152};
    if (period == 0) {
        series.revenueThousands = {92, 118, 105, 143, 169, 152, 184, 171, 202, 187, 214, 196};
        for (int hour = 0; hour < 24; hour += 2) {
            series.labels.append(QObject::tr("今日 %1:00").arg(hour, 2, 10, QLatin1Char('0')));
        }
    } else if (period == 1) {
        series.revenueThousands = {124, 155, 137, 181, 163, 205, 188};
        series.labels = {QObject::tr("周一"), QObject::tr("周二"), QObject::tr("周三"),
                         QObject::tr("周四"), QObject::tr("周五"), QObject::tr("周六"),
                         QObject::tr("周日")};
    } else if (period == 3) {
        const QDate start = customStartDate.isValid() ? customStartDate : QDate(2025, 5, 18);
        const QDate end = customEndDate.isValid() ? customEndDate : QDate(2025, 5, 31);
        for (QDate date = start; date <= end; date = date.addDays(1)) {
            const int seed = date.dayOfYear() * 17 + date.month() * 31;
            series.revenueThousands.append(88 + seed % 112);
            series.labels.append(date.toString(QStringLiteral("MM-dd")));
        }
    } else {
        series.revenueThousands = monthlyRevenue;
        const QDate start(2025, 4, 28);
        for (int index = 0; index < monthlyRevenue.size(); ++index) {
            series.labels.append(start.addDays(index).toString(QStringLiteral("MM-dd")));
        }
    }
    for (const qreal revenue : std::as_const(series.revenueThousands)) {
        // This is an independent count metric, rather than a second ambiguous money amount.
        series.completedOrderCount.append(qRound(revenue * 3.7 + 48));
    }
    return series;
}

QRectF trendPlotRect(const QWidget* widget)
{
    return widget->rect().adjusted(76, 15, -14, -50);
}

int visiblePointCount(const QRectF& plot, int total)
{
    return qBound(2, qMin(total, qMax(2, qFloor(plot.width() / 68.0) + 1)), total);
}

} // namespace

RevenueTrendWidget::RevenueTrendWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(248);
    setCursor(Qt::OpenHandCursor);
    setAccessibleName(tr("营收趋势图，可左右拖动查看全部时间点"));
}

void RevenueTrendWidget::setPeriod(int period)
{
    period_ = period;
    firstVisibleIndex_ = 0;
    selectedIndex_ = -1;
    update();
}

void RevenueTrendWidget::setDisplayMode(int displayMode)
{
    displayMode_ = displayMode;
    update();
}

void RevenueTrendWidget::setCustomDateRange(const QDate& startDate, const QDate& endDate)
{
    customStartDate_ = startDate;
    customEndDate_ = endDate;
    period_ = 3;
    firstVisibleIndex_ = 0;
    selectedIndex_ = -1;
    update();
}

void RevenueTrendWidget::setServiceSeries(const QStringList& labels, const QVector<qint64>& revenueCents,
                                          const QVector<int>& completedOrders)
{
    if (labels.size() != revenueCents.size() || labels.size() != completedOrders.size()) return;
    serviceLabels_ = labels;
    serviceRevenueCents_ = revenueCents;
    serviceCompletedOrders_ = completedOrders;
    firstVisibleIndex_ = 0;
    selectedIndex_ = -1;
    update();
}

void RevenueTrendWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = trendPlotRect(this);
    const QColor gridColor("#edf1f7");
    TrendSeries series = createTrendSeries(period_, customStartDate_, customEndDate_);
    if (!serviceLabels_.isEmpty()) {
        // A real response replaces the Mock series completely.  Keeping the
        // Mock values here made the value vector longer than the service label
        // vector, so the tooltip could index past its last label while painting.
        series.labels = serviceLabels_;
        series.revenueThousands.clear();
        series.completedOrderCount.clear();
        for (const auto cents : serviceRevenueCents_) series.revenueThousands.append(cents / 1000.0);
        for (const auto count : serviceCompletedOrders_) series.completedOrderCount.append(count);
    }
    const QList<qreal>& values = displayMode_ == 0 ? series.revenueThousands : series.completedOrderCount;
    if (values.size() < 2 || plot.width() <= 0.0 || plot.height() <= 0.0) {
        return;
    }

    const int shownCount = visiblePointCount(plot, values.size());
    const int maxFirstIndex = qMax(0, values.size() - shownCount);
    firstVisibleIndex_ = qBound(0, firstVisibleIndex_, maxFirstIndex);
    if (selectedIndex_ < firstVisibleIndex_ || selectedIndex_ >= firstVisibleIndex_ + shownCount) {
        selectedIndex_ = firstVisibleIndex_ + shownCount - 1;
    }

    const qreal maximumValue = displayMode_ == 0
                                   ? qMax<qreal>(250.0, *std::max_element(values.cbegin(), values.cend()) * 1.15)
                                   : qMax<qreal>(900.0, *std::max_element(values.cbegin(), values.cend()) * 1.15);
    QFont labelFont = painter.font();
    labelFont.setPixelSize(13);
    painter.setFont(labelFont);
    for (int index = 0; index <= 5; ++index) {
        const qreal y = plot.top() + plot.height() * index / 5.0;
        painter.setPen(QPen(gridColor, 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const qreal value = maximumValue * (5 - index) / 5.0;
        painter.setPen(QColor("#596a84"));
        const QString label = displayMode_ == 0
                                  ? QString::number(value, 'f', 0) + tr("k")
                                  : QString::number(value, 'f', 0);
        painter.drawText(QRectF(0, y - 10, plot.left() - 12, 20), Qt::AlignRight | Qt::AlignVCenter, label);
    }

    const qreal step = plot.width() / (shownCount - 1);
    const auto pointAt = [&](int seriesIndex) {
        const int visibleIndex = seriesIndex - firstVisibleIndex_;
        return QPointF(plot.left() + step * visibleIndex,
                       plot.bottom() - plot.height() * values.at(seriesIndex) / maximumValue);
    };
    QPainterPath line;
    for (int index = firstVisibleIndex_; index < firstVisibleIndex_ + shownCount; ++index) {
        const QPointF point = pointAt(index);
        index == firstVisibleIndex_ ? line.moveTo(point) : line.lineTo(point);
    }
    QPainterPath fill = line;
    fill.lineTo(plot.right(), plot.bottom());
    fill.lineTo(plot.left(), plot.bottom());
    fill.closeSubpath();
    QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
    gradient.setColorAt(0, QColor(52, 124, 246, 82));
    gradient.setColorAt(1, QColor(52, 124, 246, 2));
    painter.fillPath(fill, gradient);
    painter.setPen(QPen(QColor("#347cf6"), 2.0));
    painter.drawPath(line);
    painter.setBrush(QColor("#347cf6"));
    painter.setPen(Qt::NoPen);
    for (int index = firstVisibleIndex_; index < firstVisibleIndex_ + shownCount; ++index) {
        painter.drawEllipse(pointAt(index), 3.2, 3.2);
    }

    const QPointF selectedPoint = pointAt(selectedIndex_);
    painter.setPen(QPen(QColor("#cbd9ed"), 1, Qt::DashLine));
    painter.drawLine(QPointF(selectedPoint.x(), plot.top()), QPointF(selectedPoint.x(), plot.bottom()));
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(QColor("#347cf6"));
    painter.drawEllipse(selectedPoint, 5.0, 5.0);

    const QRectF tooltip(qBound(plot.left() + 4.0, selectedPoint.x() + 14.0, plot.right() - 202.0),
                         plot.top() + 4, 198, 64);
    painter.setPen(QPen(QColor("#e1e8f2"), 1));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(tooltip, 5, 5);
    painter.setPen(QColor("#7a879b"));
    painter.drawText(tooltip.adjusted(12, 6, -8, -34), Qt::AlignLeft | Qt::AlignVCenter,
                     series.labels.at(selectedIndex_));
    painter.setPen(QColor("#347cf6"));
    painter.setBrush(QColor("#347cf6"));
    painter.drawEllipse(QPointF(tooltip.left() + 15, tooltip.bottom() - 18), 3, 3);
    painter.setPen(QColor("#68758a"));
    const QString valueText = displayMode_ == 0
                                  ? tr("¥ %1").arg(QLocale().toString(values.at(selectedIndex_) * 1000, 'f', 0))
                                  : tr("%1 笔").arg(QLocale().toString(values.at(selectedIndex_), 'f', 0));
    const QString metricText = displayMode_ == 0 ? tr("营收") : tr("完成订单");
    painter.drawText(tooltip.adjusted(28, 26, -6, -5), Qt::AlignLeft | Qt::AlignVCenter,
                     metricText + tr("   ") + valueText);

    painter.setPen(QColor("#52637e"));
    for (int index = firstVisibleIndex_; index < firstVisibleIndex_ + shownCount; ++index) {
        const QPointF point = pointAt(index);
        QString axisLabel = series.labels.at(index);
        if (axisLabel.startsWith(tr("今日 "))) {
            axisLabel.remove(0, 3);
        }
        painter.drawText(QRectF(point.x() - 32, plot.bottom() + 10, 64, 18), Qt::AlignCenter,
                         axisLabel);
    }

    const QRectF track(plot.left(), plot.bottom() + 34, plot.width(), 4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#e8eef7"));
    painter.drawRoundedRect(track, 2, 2);
    if (shownCount < values.size()) {
        const qreal handleWidth = qMax<qreal>(42.0, track.width() * shownCount / values.size());
        const qreal handleLeft = track.left() + (track.width() - handleWidth) * firstVisibleIndex_ / maxFirstIndex;
        painter.setBrush(QColor("#9cbef3"));
        painter.drawRoundedRect(QRectF(handleLeft, track.top(), handleWidth, track.height()), 2, 2);
    }
}

void RevenueTrendWidget::mousePressEvent(QMouseEvent* event)
{
    const QRectF plot = trendPlotRect(this);
    const QRectF track(plot.left(), plot.bottom() + 34, plot.width(), 4);
    if (event->button() != Qt::LeftButton
        || (!plot.contains(event->pos()) && !track.adjusted(-6, -8, 6, 8).contains(event->pos()))) {
        return;
    }
    dragging_ = true;
    draggingOverview_ = track.adjusted(-6, -8, 6, 8).contains(event->pos());
    dragStartX_ = event->pos().x();
    dragStartFirstIndex_ = firstVisibleIndex_;
    setCursor(Qt::ClosedHandCursor);
    mouseMoveEvent(event);
}

void RevenueTrendWidget::mouseMoveEvent(QMouseEvent* event)
{
    TrendSeries series = createTrendSeries(period_, customStartDate_, customEndDate_);
    if (!serviceLabels_.isEmpty()) {
        // Keep interaction math aligned with the same service-only series used
        // by paintEvent; otherwise a drag can reintroduce a stale Mock index.
        series.labels = serviceLabels_;
        series.revenueThousands.clear();
        series.completedOrderCount.clear();
        for (const auto cents : serviceRevenueCents_) series.revenueThousands.append(cents / 1000.0);
        for (const auto count : serviceCompletedOrders_) series.completedOrderCount.append(count);
    }
    const QRectF plot = trendPlotRect(this);
    if (series.revenueThousands.size() < 2) {
        return;
    }
    const int shownCount = visiblePointCount(plot, series.revenueThousands.size());
    const int maxFirstIndex = qMax(0, series.revenueThousands.size() - shownCount);
    if (dragging_) {
        if (draggingOverview_ && maxFirstIndex > 0) {
            const qreal trackWidth = plot.width();
            const qreal handleWidth = qMax<qreal>(42.0, trackWidth * shownCount / series.revenueThousands.size());
            const qreal handleLeft = qBound(plot.left(), event->pos().x() - handleWidth / 2.0,
                                             plot.right() - handleWidth);
            firstVisibleIndex_ = qRound((handleLeft - plot.left()) * maxFirstIndex
                                        / (trackWidth - handleWidth));
        } else {
            const qreal step = plot.width() / (shownCount - 1);
            firstVisibleIndex_ = qBound(0, dragStartFirstIndex_ - qRound((event->pos().x() - dragStartX_) / step),
                                      maxFirstIndex);
        }
    }
    if (plot.contains(event->pos())) {
        const qreal step = plot.width() / (shownCount - 1);
        selectedIndex_ = qBound(firstVisibleIndex_, firstVisibleIndex_ + qRound((event->pos().x() - plot.left()) / step),
                                firstVisibleIndex_ + shownCount - 1);
    }
    update();
}

void RevenueTrendWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        mouseMoveEvent(event);
        dragging_ = false;
        draggingOverview_ = false;
        setCursor(Qt::OpenHandCursor);
    }
}

DeviceStatusWidget::DeviceStatusWidget(QWidget* parent) : QWidget(parent)
{
    setFixedSize(172, 172);
}

void DeviceStatusWidget::setCounts(int online, int offline, int fault)
{
    online_ = qMax(0, online);
    offline_ = qMax(0, offline);
    fault_ = qMax(0, fault);
    update();
}

void DeviceStatusWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF ring = rect().adjusted(19, 19, -19, -19);
    QPen pen;
    pen.setWidth(28);
    pen.setCapStyle(Qt::FlatCap);
    const int total = online_ + offline_ + fault_;
    const QList<QPair<QColor, qreal>> segments = total > 0
        ? QList<QPair<QColor, qreal>>{{QColor("#43c7bc"), 100.0 * online_ / total},
                                      {QColor("#c2cad5"), 100.0 * offline_ / total},
                                      {QColor("#f5a130"), 100.0 * fault_ / total}}
        : QList<QPair<QColor, qreal>>{{QColor("#dfe6f0"), 100.0}};
    int start = 90 * 16;
    for (const auto& segment : segments) {
        pen.setColor(segment.first);
        painter.setPen(pen);
        const int span = -qRound(segment.second * 360.0 * 16.0 / 100.0);
        painter.drawArc(ring, start, span);
        start += span;
    }
}

} // namespace charging::server
