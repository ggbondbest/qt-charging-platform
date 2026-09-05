#include "charging/client/widgets/motion.h"

#include <QAbstractButton>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QPointer>
#include <QPropertyAnimation>
#include <QTimer>

namespace charging::client::motion {

namespace {

constexpr const char* kEntryAnimName = "motionEntryAnim";
constexpr const char* kBreathAnimName = "motionBreathAnim";

// 一次性入场/脉冲动画：跑完即摘特效，控件不留合成层。
void playOpacityIn(QWidget* widget, qreal from, int ms, int delayMs)
{
    auto* effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(from);
    widget->setGraphicsEffect(effect);

    QPropertyAnimation* previous = widget->findChild<QPropertyAnimation*>(
        QString::fromLatin1(kEntryAnimName));
    if (previous != nullptr) {
        previous->stop();
        previous->deleteLater();
    }

    auto* anim = new QPropertyAnimation(effect, "opacity", widget);
    anim->setObjectName(QString::fromLatin1(kEntryAnimName));
    anim->setDuration(ms);
    anim->setStartValue(from);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    // 动画父子是控件（不是特效），finished 里才能安全地把特效摘掉。
    QObject::connect(anim, &QPropertyAnimation::finished, widget, [widget]() {
        widget->setGraphicsEffect(nullptr);
    });
    // Qt 动画没有 startDelay：用一次性定时器投递启动（父对象是动画，
    // 动画随控件销毁则投递自然作废）。
    if (delayMs > 0) {
        QTimer::singleShot(delayMs, anim, [anim]() {
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        });
    } else {
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

QGraphicsOpacityEffect* ensureBreathEffect(QWidget* widget)
{
    if (auto* existing = qobject_cast<QGraphicsOpacityEffect*>(widget->graphicsEffect())) {
        return existing;
    }
    auto* effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(1.0);
    widget->setGraphicsEffect(effect);
    return effect;
}

} // namespace

bool animationsEnabled()
{
    static const bool enabled = []() {
        if (qEnvironmentVariableIsSet("MOTION_REDUCED") &&
            qEnvironmentVariable("MOTION_REDUCED") == QLatin1String("1")) {
            return false;
        }
        // offscreen（ctest/截图）：逐帧时序会引入 flaky 与渲染差异，一律关停。
        return QGuiApplication::platformName() != QLatin1String("offscreen");
    }();
    return enabled;
}

void fadeIn(QWidget* widget, int ms, int delayMs)
{
    if (widget == nullptr || !animationsEnabled()) {
        return;
    }
    playOpacityIn(widget, 0.0, ms, delayMs);
}

void valueUpdate(QWidget* widget)
{
    if (widget == nullptr || !animationsEnabled()) {
        return;
    }
    playOpacityIn(widget, 0.35, duration::value, 0);
}

void startBreathing(QWidget* widget)
{
    if (widget == nullptr || !animationsEnabled()) {
        return;
    }
    if (widget->findChild<QPropertyAnimation*>(QString::fromLatin1(kBreathAnimName)) !=
        nullptr) {
        return; // 幂等：已在呼吸。
    }
    QGraphicsOpacityEffect* effect = ensureBreathEffect(widget);
    auto* anim = new QPropertyAnimation(effect, "opacity", widget);
    anim->setObjectName(QString::fromLatin1(kBreathAnimName));
    anim->setDuration(duration::breathe);
    anim->setLoopCount(-1);
    anim->setKeyValueAt(0.0, 1.0);
    anim->setKeyValueAt(0.5, 0.72);
    anim->setKeyValueAt(1.0, 1.0);
    anim->setEasingCurve(QEasingCurve::InOutSine);
    anim->start();
}

void stopBreathing(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    QPropertyAnimation* anim = widget->findChild<QPropertyAnimation*>(
        QString::fromLatin1(kBreathAnimName));
    if (anim == nullptr) {
        return;
    }
    anim->stop();
    anim->deleteLater();
    if (auto* effect = qobject_cast<QGraphicsOpacityEffect*>(widget->graphicsEffect())) {
        effect->setOpacity(1.0);
        widget->setGraphicsEffect(nullptr);
    }
}

void attachPressDip(QAbstractButton* button)
{
    if (button == nullptr || !animationsEnabled()) {
        return; // 未启动即完全零成本：不建特效、不连信号。
    }
    // pressed/released 对鼠标、触摸、空格都成立；程序 click() 不触发，
    // 所以测试里的合成点击永远看不到动效。
    QObject::connect(button, &QAbstractButton::pressed, button, [button]() {
        QGraphicsOpacityEffect* effect = ensureBreathEffect(button);
        if (QPropertyAnimation* running =
                effect->findChild<QPropertyAnimation*>(QString::fromLatin1("pressDip"),
                                                       Qt::FindDirectChildrenOnly)) {
            running->stop();
        }
        auto* anim = new QPropertyAnimation(effect, "opacity", effect);
        anim->setObjectName(QString::fromLatin1("pressDip"));
        anim->setDuration(duration::micro);
        anim->setStartValue(effect->opacity());
        anim->setEndValue(0.85); // 微沉：够摸到、不显眼。
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    });
    QObject::connect(button, &QAbstractButton::released, button, [button]() {
        auto* effect = qobject_cast<QGraphicsOpacityEffect*>(button->graphicsEffect());
        if (effect == nullptr) {
            return;
        }
        QPropertyAnimation* previous =
            effect->findChild<QPropertyAnimation*>(QString::fromLatin1("pressDip"),
                                                   Qt::FindDirectChildrenOnly);
        if (previous != nullptr) {
            previous->stop();
        }
        auto* anim = new QPropertyAnimation(effect, "opacity", effect);
        anim->setObjectName(QString::fromLatin1("pressDip"));
        anim->setDuration(duration::enter);
        anim->setStartValue(effect->opacity());
        anim->setEndValue(1.0);
        anim->setEasingCurve(QEasingCurve::OutBack); // 回弹，过冲被透明度截断，
        anim->start(QAbstractAnimation::DeleteWhenStopped); // 恰是"弹一下"的手感。
    });
}

} // namespace charging::client::motion
