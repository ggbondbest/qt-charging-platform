#pragma once

#include <QWidget>

class QPropertyAnimation;

namespace charging::client {

// Decorative "charging in progress" indicator for the live session page: a
// rotating energy ring around a pulsing lightning glyph. It carries no data
// (every real number is rendered from GET_CHARGING_STATUS elsewhere) and is
// driven purely by painting, so it touches no protocol or global stylesheet.
//
// The animation runs only while a session is actively charging AND the widget
// is visible: setActive() gates on status, show/hide events start/stop the
// timers so a hidden or finished page never repaints pointlessly.
class ChargingPulse final : public QWidget
{
    Q_OBJECT

    Q_PROPERTY(qreal ringPhase READ ringPhase WRITE setRingPhase)
    Q_PROPERTY(qreal breath READ breath WRITE setBreath)

public:
    explicit ChargingPulse(QWidget* parent = nullptr);

    void setActive(bool active);
    bool isActive() const;

    QSize sizeHint() const override;

    qreal ringPhase() const { return ringPhase_; }
    void setRingPhase(qreal value);
    qreal breath() const { return breath_; }
    void setBreath(qreal value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void startAnimations();
    void stopAnimations();

    QPropertyAnimation* ringAnim_ = nullptr;
    QPropertyAnimation* breathAnim_ = nullptr;
    qreal ringPhase_ = 0.0; // 0..1 -> 0..360 degrees of arc rotation
    qreal breath_ = 0.0;    // 0..1 -> lightning opacity/scale pulse
    bool active_ = false;
    bool visibleNow_ = false;
};

} // namespace charging::client
