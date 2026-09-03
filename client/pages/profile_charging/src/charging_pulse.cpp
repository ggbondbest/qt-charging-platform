#include "charging/client/profile_charging/charging_pulse.h"

#include <QHideEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QShowEvent>

namespace charging::client {

namespace {

// Mirrors the palette tokens in resources/qss/client_platform.qss
// (#00B578 brand green, #E2F7EC soft green wash, #9CA3AF neutral) — painted
// rather than styled so no global QSS is touched.
const QColor kRingColor(0x00, 0xB5, 0x78);
const QColor kTrackColor(0xE2, 0xF7, 0xEC);
const QColor kIdleBoltColor(0x9C, 0xA3, 0xAF);

constexpr qreal kTurnPeriodMs = 1600.0;  // one full sweep of the arc
constexpr qreal kBreathPeriodMs = 1400.0; // one dim->bright->dim breath cycle
constexpr int kBoltSpanDegrees = 100;    // length of the sweeping arc

} // namespace

ChargingPulse::ChargingPulse(QWidget* parent) : QWidget(parent)
{
    setFixedSize(52, 52);
    setAttribute(Qt::WA_TransparentForMouseEvents);

    ringAnim_ = new QPropertyAnimation(this, "ringPhase", this);
    ringAnim_->setDuration(static_cast<int>(kTurnPeriodMs));
    ringAnim_->setStartValue(0.0);
    ringAnim_->setEndValue(1.0);
    ringAnim_->setEasingCurve(QEasingCurve::Linear);
    ringAnim_->setLoopCount(-1);

    breathAnim_ = new QPropertyAnimation(this, "breath", this);
    breathAnim_->setDuration(static_cast<int>(kBreathPeriodMs));
    breathAnim_->setKeyValueAt(0.0, 0.15);
    breathAnim_->setKeyValueAt(0.5, 1.0);
    breathAnim_->setKeyValueAt(1.0, 0.15);
    breathAnim_->setEasingCurve(QEasingCurve::InOutSine);
    breathAnim_->setLoopCount(-1);
}

QSize ChargingPulse::sizeHint() const
{
    return QSize(52, 52);
}

void ChargingPulse::setActive(bool active)
{
    if (active_ == active) {
        return;
    }
    active_ = active;
    if (active_ && visibleNow_) {
        startAnimations();
    } else {
        stopAnimations();
    }
    update();
}

bool ChargingPulse::isActive() const
{
    return active_;
}

void ChargingPulse::setRingPhase(qreal value)
{
    if (qFuzzyCompare(ringPhase_, value)) {
        return;
    }
    ringPhase_ = value;
    update();
}

void ChargingPulse::setBreath(qreal value)
{
    if (qFuzzyCompare(breath_, value)) {
        return;
    }
    breath_ = value;
    update();
}

void ChargingPulse::startAnimations()
{
    if (ringAnim_->state() != QAbstractAnimation::Running) {
        ringAnim_->start();
    }
    if (breathAnim_->state() != QAbstractAnimation::Running) {
        breathAnim_->start();
    }
}

void ChargingPulse::stopAnimations()
{
    if (ringAnim_->state() == QAbstractAnimation::Running) {
        ringAnim_->stop();
    }
    if (breathAnim_->state() == QAbstractAnimation::Running) {
        breathAnim_->stop();
    }
    ringPhase_ = 0.0;
    breath_ = 1.0;
}

void ChargingPulse::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    visibleNow_ = true;
    if (active_) {
        startAnimations();
        update();
    }
}

void ChargingPulse::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    visibleNow_ = false;
    stopAnimations(); // a hidden or navigated-away page must not repaint
}

void ChargingPulse::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPointF center = rect().center() + QPointF(0.5, 0.5);
    constexpr qreal kPenWidth = 4.0;
    const qreal radius = width() / 2.0 - kPenWidth;
    const QRectF ringRect(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

    if (active_) {
        // Track ring, then the sweeping arc on top (Qt angles: 1/16 degree).
        QPen trackPen(kTrackColor);
        trackPen.setWidthF(kPenWidth);
        painter.setPen(trackPen);
        painter.drawEllipse(ringRect);

        QPen arcPen(kRingColor);
        arcPen.setWidthF(kPenWidth);
        arcPen.setCapStyle(Qt::RoundCap);
        painter.setPen(arcPen);
        const int startAngle = static_cast<int>((-90.0 + ringPhase_ * 360.0) * 16.0);
        painter.drawArc(ringRect, startAngle, -kBoltSpanDegrees * 16);
    } else {
        // Idle rest state: a faint dotted-look ring outline only.
        QPen trackPen(kTrackColor);
        trackPen.setWidthF(2.0);
        painter.setPen(trackPen);
        painter.drawEllipse(ringRect);
    }

    // Lightning bolt, breathing in opacity while active (alpha ~38%..100%).
    const qreal level = active_ ? breath_ : 1.0;
    QColor boltColor = active_ ? kRingColor : kIdleBoltColor;
    boltColor.setAlphaF(active_ ? (0.38 + 0.62 * level) : 1.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(boltColor);

    const qreal scale = active_ ? (0.92 + 0.08 * level) : 1.0;
    const qreal boltW = 15.0 * scale;
    const qreal boltH = 24.0 * scale;
    QPolygonF bolt;
    bolt << QPointF(center.x() - boltW * 0.15, center.y() - boltH / 2.0)
         << QPointF(center.x() - boltW / 2.0, center.y() + boltH * 0.12)
         << QPointF(center.x() - boltW * 0.05, center.y() + boltH * 0.12)
         << QPointF(center.x() + boltW * 0.15, center.y() + boltH / 2.0)
         << QPointF(center.x() + boltW / 2.0, center.y() - boltH * 0.12)
         << QPointF(center.x() + boltW * 0.05, center.y() - boltH * 0.12);
    painter.drawPolygon(bolt);
}

} // namespace charging::client
