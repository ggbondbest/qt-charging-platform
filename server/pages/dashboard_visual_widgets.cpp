#include "dashboard_visual_widgets.h"

#include <QLinearGradient>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QtMath>

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

RevenueTrendWidget::RevenueTrendWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(226);
}

void RevenueTrendWidget::setPeriod(int period)
{
    period_ = period;
    update();
}

void RevenueTrendWidget::setDisplayMode(int displayMode)
{
    displayMode_ = displayMode;
    update();
}

void RevenueTrendWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = rect().adjusted(74, 15, -12, -34);
    const QColor gridColor("#edf1f7");
    QList<qreal> values;
    QStringList dates;
    QString tooltipDate;
    qreal maximumValue = 250.0;
    switch (period_) {
    case 0:
        values = {92, 118, 105, 143, 169, 152, 184, 171, 202, 187, 214, 196};
        dates = {tr("08:00"), tr("10:00"), tr("12:00"), tr("14:00"), tr("16:00"), tr("18:00")};
        tooltipDate = tr("今日 16:00");
        break;
    case 1:
        values = {124, 155, 137, 181, 163, 205, 188};
        dates = {tr("周一"), tr("周二"), tr("周三"), tr("周四"), tr("周五"), tr("周六"), tr("周日")};
        tooltipDate = tr("本周六");
        break;
    case 3:
        values = {122, 108, 151, 164, 139, 171, 185, 157, 144, 176, 161, 192, 174, 203};
        dates = {tr("05-18"), tr("05-20"), tr("05-22"), tr("05-24"),
                 tr("05-26"), tr("05-28"), tr("05-30")};
        tooltipDate = tr("05-24 至 05-30");
        break;
    default:
        values = {132, 112, 91, 105, 131, 161, 169, 160, 143, 150, 165, 170,
                  153, 157, 185, 193, 177, 152, 155, 161, 132, 126, 130, 104,
                  89, 111, 125, 119, 102, 109, 154, 172, 176, 157, 152};
        dates = {tr("05-04"), tr("05-08"), tr("05-12"), tr("05-16"),
                 tr("05-20"), tr("05-24"), tr("05-28"), tr("06-01")};
        tooltipDate = tr("05-20");
        break;
    }
    if (displayMode_ == 1) {
        for (qreal& value : values) {
            value = value * 0.72 + 12.0;
        }
        maximumValue = 200.0;
    }

    const QString unit = displayMode_ == 0 ? tr("营收金额") : tr("订单金额");
    const QStringList yLabels = {QString::number(maximumValue, 'f', 0) + tr(",000"),
                                 QString::number(maximumValue * 0.8, 'f', 0) + tr(",000"),
                                 QString::number(maximumValue * 0.6, 'f', 0) + tr(",000"),
                                 QString::number(maximumValue * 0.4, 'f', 0) + tr(",000"),
                                 QString::number(maximumValue * 0.2, 'f', 0) + tr(",000"), tr("0")};
    painter.setPen(QPen(gridColor, 1, Qt::DashLine));
    QFont labelFont = painter.font();
    labelFont.setPixelSize(13);
    painter.setFont(labelFont);
    for (int index = 0; index < yLabels.size(); ++index) {
        const qreal y = plot.top() + plot.height() * index / (yLabels.size() - 1);
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(QColor("#596a84"));
        painter.drawText(QRectF(0, y - 10, plot.left() - 12, 20), Qt::AlignRight | Qt::AlignVCenter,
                         yLabels.at(index));
        painter.setPen(QPen(gridColor, 1, Qt::DashLine));
    }

    QPainterPath line;
    for (int index = 0; index < values.size(); ++index) {
        const qreal x = plot.left() + plot.width() * index / (values.size() - 1);
        const qreal y = plot.bottom() - plot.height() * values.at(index) / maximumValue;
        index == 0 ? line.moveTo(x, y) : line.lineTo(x, y);
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
    for (int index = 0; index < values.size(); ++index) {
        const qreal x = plot.left() + plot.width() * index / (values.size() - 1);
        const qreal y = plot.bottom() - plot.height() * values.at(index) / maximumValue;
        painter.drawEllipse(QPointF(x, y), 3.1, 3.1);
    }

    const int hoverIndex = values.size() / 2;
    const qreal hoverX = plot.left() + plot.width() * hoverIndex / (values.size() - 1);
    const qreal hoverY = plot.bottom() - plot.height() * values.at(hoverIndex) / maximumValue;
    painter.setPen(QPen(QColor("#cbd9ed"), 1, Qt::DashLine));
    painter.drawLine(QPointF(hoverX, plot.top()), QPointF(hoverX, plot.bottom()));
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(QColor("#347cf6"));
    painter.drawEllipse(QPointF(hoverX, hoverY), 4.6, 4.6);
    const QRectF tooltip(qMin(hoverX + 16, plot.right() - 198), plot.top() + 4, 198, 64);
    painter.setPen(QPen(QColor("#e1e8f2"), 1));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(tooltip, 5, 5);
    QFont tooltipFont = painter.font();
    tooltipFont.setPixelSize(13);
    painter.setFont(tooltipFont);
    painter.setPen(QColor("#7a879b"));
    painter.drawText(tooltip.adjusted(12, 6, -8, -34), Qt::AlignLeft | Qt::AlignVCenter,
                     tooltipDate);
    painter.setPen(QColor("#347cf6"));
    painter.drawEllipse(QPointF(tooltip.left() + 15, tooltip.bottom() - 18), 3, 3);
    painter.setPen(QColor("#68758a"));
    const QString tooltipValue = displayMode_ == 0 ? tr("¥ 142,680.00") : tr("¥ 102,740.00");
    painter.drawText(tooltip.adjusted(28, 26, -6, -5), Qt::AlignLeft | Qt::AlignVCenter,
                     unit + tr("   ") + tooltipValue);

    painter.setPen(QColor("#52637e"));
    for (int index = 0; index < dates.size(); ++index) {
        const qreal x = plot.left() + plot.width() * index / (dates.size() - 1);
        painter.drawText(QRectF(x - 32, plot.bottom() + 10, 64, 18), Qt::AlignCenter, dates.at(index));
    }
}

DeviceStatusWidget::DeviceStatusWidget(QWidget* parent) : QWidget(parent)
{
    setFixedSize(172, 172);
}

void DeviceStatusWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF ring = rect().adjusted(19, 19, -19, -19);
    QPen pen;
    pen.setWidth(28);
    pen.setCapStyle(Qt::FlatCap);
    const QList<QPair<QColor, qreal>> segments = {{QColor("#43c7bc"), 82.4},
                                                    {QColor("#c2cad5"), 11.7},
                                                    {QColor("#f5a130"), 5.9}};
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
