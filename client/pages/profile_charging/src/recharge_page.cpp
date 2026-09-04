#include "charging/client/profile_charging/recharge_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_bar.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/loading_overlay.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"
#include "charging/common/protocol/protocol.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

namespace charging::client {

namespace {

constexpr int kChipCount = 4;

} // namespace

RechargePage::RechargePage(WalletService* service, QWidget* parent)
    : QWidget(parent), service_(service)
{
    buildUi();

    connect(service_, &WalletService::rechargeCompleted, this, &RechargePage::onRechargeCompleted);
    connect(service_, &WalletService::operationFailed, this, &RechargePage::onOperationFailed);
}

void RechargePage::buildUi()
{
    // 外框：内容区 + 底部操作条（ActionBar）。主按钮钉在页面底部，
    // 不再随长内容漂到空白区中间。
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);
    auto* content = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(20, 16, 20, 16);
    rootLayout->setSpacing(14);
    outerLayout->addWidget(content, 1);

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(6);
    backButton_ = new ActionButton(ActionButton::Variant::Ghost, QStringLiteral("←"), this);
    backButton_->setFixedWidth(44);
    connect(backButton_, &ActionButton::clicked, this, &RechargePage::backRequested);
    auto* titleLabel = new QLabel(tr("充值"), this);    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    headerRow->addWidget(backButton_);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    rootLayout->addLayout(headerRow);

    auto* balanceCard = new Card(this);
    auto* balanceLayout = new QHBoxLayout();
    balanceLayout->setSpacing(8);
    auto* balanceCaption = new QLabel(tr("当前余额（元）"), balanceCard);
    balanceCaption->setProperty("role", QStringLiteral("caption"));
    balanceValueLabel_ = new QLabel(QStringLiteral("¥ --"), balanceCard);
    // 支付辅助信息降级为行内金额（34px 大余额只属于首页/钱包页的横幅）。
    balanceValueLabel_->setProperty("role", QStringLiteral("amountStrong"));
    balanceLayout->addWidget(balanceCaption);
    balanceLayout->addStretch();
    balanceLayout->addWidget(balanceValueLabel_);
    balanceCard->bodyLayout()->addLayout(balanceLayout);
    rootLayout->addWidget(balanceCard);

    auto* amountCard = new Card(this);
    auto* amountLayout = amountCard->bodyLayout();
    auto* amountTitle = new QLabel(tr("选择充值金额"), amountCard);
    amountTitle->setProperty("role", QStringLiteral("sectionTitle"));
    amountLayout->addWidget(amountTitle);

    auto* chipGrid = new QGridLayout();
    chipGrid->setSpacing(10);
    // TODO(contract): preset amounts and the maximum are UI candidates until
    // the RECHARGE bounds are frozen with the leader.
    const qint64 presets[kChipCount] = {5000, 10000, 20000, 50000}; // 50/100/200/500 元
    for (int index = 0; index < kChipCount; ++index) {
        const qint64 cents = presets[index];
        auto* chip = new ActionButton(ActionButton::Variant::Chip,
                                      QStringLiteral("¥%1").arg(cents / 100), amountCard);
        chip->setMinimumHeight(52);
        chipAmountsCents_.append(cents);
        amountChips_.append(chip);
        connect(chip, &ActionButton::toggled, this, [this, index](bool checked) {
            if (!checked) {
                return;
            }
            for (ActionButton* other : amountChips_) {
                if (other != amountChips_.at(index)) {
                    other->setChecked(false);
                }
            }
            customAmountEdit_->clear();
        });
        chipGrid->addWidget(chip, index / 2, index % 2);
    }
    amountLayout->addLayout(chipGrid);

    customAmountEdit_ = new QLineEdit(amountCard);
    customAmountEdit_->setPlaceholderText(tr("其他金额（元），最多 99999.99"));
    customAmountEdit_->setClearButtonEnabled(true);
    customAmountEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^(0|[1-9][0-9]{0,7})(\\.[0-9]{0,2})?$")),
        customAmountEdit_));
    // Typing a custom amount deselects the chips.
    connect(customAmountEdit_, &QLineEdit::textEdited, this, [this]() {
        for (ActionButton* chip : amountChips_) {
            chip->setChecked(false);
        }
    });
    amountLayout->addWidget(customAmountEdit_);
    // 默认选中 ¥100：让选择态与选中样式从一开始就有落点。
    amountChips_.at(1)->setChecked(true);

    rootLayout->addWidget(amountCard);
    rootLayout->addStretch();

    auto* actionBar = new ActionBar(ActionBar::Variant::Primary, tr("确认充值"), this);
    outerLayout->addWidget(actionBar);
    confirmButton_ = actionBar->actionButton();
    connect(confirmButton_, &ActionButton::clicked, this, &RechargePage::onConfirmClicked);

    overlay_ = new LoadingOverlay(this);
    overlay_->setVisible(false);
}

void RechargePage::setBalance(qint64 balanceCents)
{
    balanceValueLabel_->setText(QStringLiteral("¥ %1").arg(formatCentsAsYuan(balanceCents)));
}

void RechargePage::setEmbedded(bool embedded)
{
    // 全局顶部导航已提供返回，隐藏页内返回按钮。
    backButton_->setVisible(!embedded);
}

void RechargePage::resetForm()
{
    for (ActionButton* chip : amountChips_) {
        chip->setChecked(false);
    }
    customAmountEdit_->clear();
    amountChips_.at(1)->setChecked(true); // 回到默认 ¥100
}

qint64 RechargePage::selectedAmountCents(QString* invalidReason) const
{
    const QString customText = customAmountEdit_->text().trimmed();
    if (!customText.isEmpty()) {
        qint64 cents = 0;
        if (!parseYuanTextToCents(customText, &cents)) {
            if (invalidReason != nullptr) {
                *invalidReason = tr("请输入正确的充值金额（最多两位小数）");
            }
            return -1;
        }
        if (cents > WalletService::kMaximumRechargeCents) {
            if (invalidReason != nullptr) {
                *invalidReason = tr("单笔充值不能超过 ¥%1")
                                     .arg(formatCentsAsYuan(
                                         WalletService::kMaximumRechargeCents));
            }
            return -1;
        }
        return cents;
    }

    for (int index = 0; index < amountChips_.size(); ++index) {
        if (amountChips_.at(index)->isChecked()) {
            return chipAmountsCents_.at(index);
        }
    }

    if (invalidReason != nullptr) {
        *invalidReason = tr("请选择或输入充值金额");
    }
    return -1;
}

void RechargePage::onConfirmClicked()
{
    if (service_->isRecharging()) {
        return; // One recharge at a time; the server is authoritative.
    }

    QString invalidReason;
    const qint64 amountCents = selectedAmountCents(&invalidReason);
    if (amountCents < 0) {
        Toast::show(this, invalidReason, StatusTag::Tone::Warning);
        return;
    }

    QMessageBox box(QMessageBox::Question, tr("确认充值"),
                    tr("确认为账户充值 ¥%1？").arg(formatCentsAsYuan(amountCents)),
                    QMessageBox::NoButton, this);
    QPushButton* confirm = box.addButton(tr("确认"), QMessageBox::AcceptRole);
    box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != confirm) {
        return;
    }

    setSubmitting(true);
    service_->recharge(amountCents);
}

void RechargePage::setSubmitting(bool submitting)
{
    confirmButton_->setEnabled(!submitting);
    backButton_->setEnabled(!submitting);
    customAmountEdit_->setEnabled(!submitting);
    for (ActionButton* chip : amountChips_) {
        chip->setEnabled(!submitting);
    }
    if (submitting) {
        overlay_->showFor();
    } else {
        overlay_->hideFor();
    }
}

void RechargePage::onRechargeCompleted(qint64 amountCents, qint64 balanceAfterCents)
{
    setSubmitting(false);
    resetForm();
    setBalance(balanceAfterCents);
    Toast::show(this, tr("充值成功 +%1 元").arg(formatCentsAsYuan(amountCents)),
                StatusTag::Tone::Success);
    emit rechargeSucceeded(balanceAfterCents);
}

void RechargePage::onOperationFailed(const QString& type,
                                     const charging::protocol::ProtocolError& error)
{
    const QString rechargeType =
        QString::fromLatin1(charging::protocol::request_type::kRecharge);
    if (type != rechargeType) {
        return; // Other pages own their own request failures.
    }
    setSubmitting(false);
    Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);
}

} // namespace charging::client
