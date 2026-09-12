#pragma once

#include "charging/client/widgets/card.h"

namespace charging::client {

// Card that emits clicked() when the user releases the mouse inside it.
// Used by list rows (orders, recharge records) that open a detail page.
class ClickableCard final : public Card
{
    Q_OBJECT

public:
    explicit ClickableCard(QWidget* parent = nullptr);

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
};

} // namespace charging::client
