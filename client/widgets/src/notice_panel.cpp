#include "charging/client/widgets/notice_panel.h"

#include "charging/client/widgets/action_button.h"

#include <QFont>
#include <QLabel>
#include <QVBoxLayout>

namespace charging::client {

NoticePanel::NoticePanel(const QString& glyph, const QString& title, const QString& description,
                         const QString& actionText, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("uiNoticePanel"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 28, 16, 28);
    layout->setSpacing(6);

    glyphLabel_ = new QLabel(this);
    glyphLabel_->setObjectName(QStringLiteral("uiNoticeGlyph"));
    glyphLabel_->setAlignment(Qt::AlignCenter);
    QFont glyphFont = glyphLabel_->font();
    glyphFont.setPointSize(22);
    glyphLabel_->setFont(glyphFont);

    titleLabel_ = new QLabel(this);
    titleLabel_->setObjectName(QStringLiteral("uiNoticeTitle"));
    titleLabel_->setAlignment(Qt::AlignCenter);
    titleLabel_->setWordWrap(true);
    QFont titleFont = titleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPointSize(11);
    titleLabel_->setFont(titleFont);

    descriptionLabel_ = new QLabel(this);
    descriptionLabel_->setObjectName(QStringLiteral("uiNoticeDescription"));
    descriptionLabel_->setAlignment(Qt::AlignCenter);
    descriptionLabel_->setWordWrap(true);

    layout->addWidget(glyphLabel_);
    layout->addWidget(titleLabel_);
    layout->addWidget(descriptionLabel_);

    actionButton_ = new ActionButton(ActionButton::Variant::Secondary, actionText, this);
    actionButton_->setMaximumWidth(160);
    QObject::connect(actionButton_, &ActionButton::clicked, this, &NoticePanel::actionTriggered);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    buttonRow->addWidget(actionButton_);
    buttonRow->addStretch();
    layout->addSpacing(6);
    layout->addLayout(buttonRow);

    setContent(glyph, title, description, actionText);
}

void NoticePanel::setContent(const QString& glyph, const QString& title,
                             const QString& description, const QString& actionText)
{
    glyphLabel_->setText(glyph);
    titleLabel_->setText(title);
    descriptionLabel_->setText(description);
    actionButton_->setVisible(!actionText.isEmpty());
    if (!actionText.isEmpty()) {
        actionButton_->setText(actionText);
    }
}

} // namespace charging::client
