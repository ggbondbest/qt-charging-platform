#include "charging/client/widgets/clickable_card.h"

#include <QMouseEvent>

namespace charging::client {

ClickableCard::ClickableCard(QWidget* parent) : Card(parent)
{
    setCursor(Qt::PointingHandCursor);
}

void ClickableCard::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        emit clicked();
    }
    Card::mouseReleaseEvent(event);
}

} // namespace charging::client
