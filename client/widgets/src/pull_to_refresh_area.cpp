#include "charging/client/widgets/pull_to_refresh_area.h"

#include "charging/client/widgets/motion.h"

#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>

namespace charging::client {

namespace {
// 阻力曲线：raw 像素下拉映射为让位高度，越拉越沉（90px 处约 63%）。
int rubberGap(double rawPx)
{
    if (rawPx <= 0.0) {
        return 0;
    }
    return qRound(90.0 * (1.0 - qExp(-rawPx / 90.0)));
}
} // namespace

PullToRefreshArea::PullToRefreshArea(QWidget* parent) : QScrollArea(parent)
{
    // 垫块是容器布局的第 0 项（setPullContent 时插入），初始完全收起。
    spacer_ = new QWidget(this);
    spacer_->setFixedHeight(0);

    pill_ = new QLabel(viewport());
    pill_->setObjectName(QStringLiteral("uiPullPill"));
    pill_->setAlignment(Qt::AlignCenter);
    pill_->hide();
}

void PullToRefreshArea::setPullContent(QWidget* widget)
{
    auto* layout = qobject_cast<QVBoxLayout*>(widget->layout());
    if (layout != nullptr) {
        layout->insertWidget(0, spacer_);
    }
    setWidget(widget);
}

void PullToRefreshArea::setState(State state)
{
    state_ = state;
    updatePill();
}

void PullToRefreshArea::setGap(int px, bool animate)
{
    const int target = qBound(0, px, 120);
    if (!animate || !motion::animationsEnabled()) {
        spacer_->setFixedHeight(target);
        return;
    }
    auto* anim = new QVariantAnimation(spacer_);
    anim->setDuration(motion::duration::enter);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(spacer_->height());
    anim->setEndValue(target);
    QObject::connect(anim, &QVariantAnimation::valueChanged, spacer_,
                     [this](const QVariant& value) {
                         spacer_->setFixedHeight(value.toInt());
                     });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void PullToRefreshArea::updatePill()
{
    QString text;
    switch (state_) {
    case State::Collapsed:
        pill_->hide();
        return;
    case State::Pulling:
        text = tr("⌄ 下拉刷新");
        break;
    case State::Armed:
        text = tr("⌃ 松开刷新");
        break;
    case State::Refreshing:
        text = tr("⟳ 正在刷新…");
        break;
    }
    pill_->setText(text);
    pill_->adjustSize();
    pill_->move((viewport()->width() - pill_->width()) / 2,
                qMax(6, spacer_->height() - pill_->height() - 8));
    pill_->raise();
    pill_->show();
}

bool PullToRefreshArea::viewportEvent(QEvent* event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        // 只在滚动条已顶天、纯左键、且当前不在刷新时武装。按下点若是按钮，
        // 事件根本不会传播到 viewport（按钮自吞 press），无需再排除。
        pending_ = mouse->button() == Qt::LeftButton && state_ == State::Collapsed &&
                   verticalScrollBar()->value() <= verticalScrollBar()->minimum();
        pressY_ = mouse->pos().y();
        break;
    }
    case QEvent::MouseMove: {
        if (!pending_ && !pulling_) {
            break;
        }
        const int dy = static_cast<QMouseEvent*>(event)->pos().y() - pressY_;
        if (!pulling_) {
            if (dy < kActivatePx) {
                break; // 还可能是点击/斜向滑，不抢。
            }
            beginPull();
        }
        setState(dy >= kThresholdPx ? State::Armed : State::Pulling);
        setGap(rubberGap(dy));
        break;
    }
    case QEvent::MouseButtonRelease: {
        if (pulling_) {
            finishGesture(state_ == State::Armed);
            event->accept();
            return true;
        }
        pending_ = false; // 普通点击收场。
        break;
    }
    default:
        break;
    }
    return QScrollArea::viewportEvent(event);
}

void PullToRefreshArea::beginPull()
{
    pulling_ = true;
    pending_ = false;
    // 卡片会吞掉 release 并把它当点击——拖拽期间在应用层抢在卡片之前吃 release。
    if (!appFiltered_) {
        qApp->installEventFilter(this);
        appFiltered_ = true;
    }
}

void PullToRefreshArea::endPullInterceptor()
{
    if (appFiltered_) {
        qApp->removeEventFilter(this);
        appFiltered_ = false;
    }
}

bool PullToRefreshArea::eventFilter(QObject* watched, QEvent* event)
{
    if (pulling_ && event->type() == QEvent::MouseButtonRelease) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget != nullptr && widget->window() == window()) {
            finishGesture(state_ == State::Armed);
            return true; // 吞掉：本次拖拽不算任何点击。
        }
    }
    return QScrollArea::eventFilter(watched, event);
}

void PullToRefreshArea::finishGesture(bool triggerRefresh)
{
    endPullInterceptor();
    pulling_ = false;
    pending_ = false;
    if (triggerRefresh) {
        setState(State::Refreshing);
        setGap(kRestGapPx, true);
        emit refreshRequested();
    } else {
        setState(State::Collapsed);
        setGap(0, true);
    }
}

void PullToRefreshArea::setRefreshing(bool refreshing)
{
    if (refreshing) {
        if (state_ != State::Refreshing) {
            setState(State::Refreshing);
            setGap(kRestGapPx);
        }
        return;
    }
    if (state_ == State::Refreshing) {
        setState(State::Collapsed);
        if (motion::animationsEnabled()) {
            // 数据刚到：停半拍再收起，胶囊读得到"正在刷新"。
            QTimer::singleShot(350, this, [this]() { setGap(0, true); });
        } else {
            setGap(0);
        }
    }
}

} // namespace charging::client
