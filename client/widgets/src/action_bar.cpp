#include "charging/client/widgets/action_bar.h"

#include "charging/client/widgets/action_button.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace charging::client {

ActionBar::ActionBar(Variant variant, const QString& text, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("uiActionBar"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 10, 20, 14);
    root->setSpacing(8);

    captionLabel_ = new QLabel(this);
    captionLabel_->setObjectName(QStringLiteral("uiActionCaption"));
    captionLabel_->setAlignment(Qt::AlignCenter);
    captionLabel_->setVisible(false);
    root->addWidget(captionLabel_);

    button_ = new ActionButton(
        variant == Variant::Danger ? ActionButton::Variant::Danger
                                   : ActionButton::Variant::Primary,
        text, this);
    root->addWidget(button_);
}

ActionButton* ActionBar::actionButton() const
{
    return button_;
}

void ActionBar::setActionText(const QString& text)
{
    button_->setText(text);
}

void ActionBar::setCaption(const QString& text)
{
    captionLabel_->setText(text);
    captionLabel_->setVisible(!text.isEmpty());
}

} // namespace charging::client
