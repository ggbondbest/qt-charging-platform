#include "charging/client/widgets/loading_overlay.h"

#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>

namespace charging::client {

// Lightweight arc spinner. QBusyIndicator does not exist in the Qt 6.2.4
// baseline, so the overlay paints its own indicator with documented 6.2 API.
// Internal to the widgets module; only forward-declared in the header.
class Spinner final : public QWidget
{
public:
    explicit Spinner(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(36, 36);
        connect(&timer_, &QTimer::timeout, this, [this]() {
            angle_ = (angle_ + 18) % 360;
            update();
        });
    }

    void startSpinning()
    {
        timer_.start(40);
    }

    void stopSpinning()
    {
        timer_.stop();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QPen pen(QColor(0, 181, 120));
        pen.setWidth(4);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);

        const QRectF arcRect = rect().adjusted(2, 2, -2, -2);
        painter.drawArc(arcRect, angle_ * 16, 280 * 16);
    }

private:
    QTimer timer_;
    int angle_ = 0;
};

LoadingOverlay::LoadingOverlay(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("uiLoadingOverlay"));
    hide();

    spinner_ = new Spinner(this);

    if (parent != nullptr) {
        parent->installEventFilter(this);
        resize(parent->size());
    }
    centerSpinner();
}

void LoadingOverlay::showFor()
{
    if (parentWidget() != nullptr) {
        resize(parentWidget()->size());
    }
    centerSpinner();
    raise();
    show();
    spinner_->startSpinning();
}

void LoadingOverlay::hideFor()
{
    spinner_->stopSpinning();
    hide();
}

bool LoadingOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        resize(parentWidget()->size());
        centerSpinner();
    }
    return QWidget::eventFilter(watched, event);
}

void LoadingOverlay::centerSpinner()
{
    spinner_->move((width() - spinner_->width()) / 2, (height() - spinner_->height()) / 2);
}

void LoadingOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(244, 246, 248, 205));
}

} // namespace charging::client
