#include "charging/client/widgets/card.h"

#include <QVBoxLayout>

namespace charging::client {

Card::Card(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("uiCard"));

    bodyLayout_ = new QVBoxLayout(this);
    bodyLayout_->setContentsMargins(18, 16, 18, 16);
    bodyLayout_->setSpacing(10);
}

QVBoxLayout* Card::bodyLayout() const
{
    return bodyLayout_;
}

} // namespace charging::client
