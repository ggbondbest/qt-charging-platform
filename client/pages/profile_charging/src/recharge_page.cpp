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
#include <QShowEvent>
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

    // 未确认充值恢复条：契约 v1 §3 下超时/断线不是明确失败，持久化的
    // {amountCents, transactionNo} 意图需要醒目入口"按原金额重试"幂等确认，
    // 而不是让用户对着 toast 猜测下一步（§5 验收：恢复提示优化）。
    pendingBar_ = new QWidget(content);
    pendingBar_->setObjectName(QStringLiteral("rechargePendingBar"));
    auto* pendingLayout = new QHBoxLayout(pendingBar_);
    pendingLayout->setContentsMargins(12, 8, 12, 8);
    pendingLayout->setSpacing(8);
    auto* pendingPill = new StatusTag(tr("待确认"), StatusTag::Tone::Warning, pendingBar_);
    pendingNoticeLabel_ = new QLabel(pendingBar_);
    pendingNoticeLabel_->setObjectName(QStringLiteral("rechargePendingNotice"));
    pendingNoticeLabel_->setProperty("role", QStringLiteral("secondary"));
    pendingNoticeLabel_->setWordWrap(true);
    retryButton_ = new ActionButton(ActionButton::Variant::Chip, tr("按原金额重试"), pendingBar_);
    retryButton_->setObjectName(QStringLiteral("rechargeRetryButton"));
    retryButton_->setMinimumHeight(40);
    connect(retryButton_, &ActionButton::clicked, this, [this]() {
        const qint64 pending = service_->pendingRechargeAmount();
        if (pending <= 0 || service_->isRecharging()) {
            return;
        }
        setSubmitting(true);
        service_->recharge(pending); // 同金额命中流水号复用路径，服务端幂等去重。
    });
    pendingLayout->addWidget(pendingPill);
    pendingLayout->addWidget(pendingNoticeLabel_, 1);
    pendingLayout->addWidget(retryButton_);
    pendingBar_->setVisible(false);
    rootLayout->addWidget(pendingBar_);

    auto* amountCard = new Card(this);
    auto* amountLayout = amountCard->bodyLayout();
    auto* amountTitle = new QLabel(tr("选择充值金额"), amountCard);
    amountTitle->setProperty("role", QStringLiteral("sectionTitle"));
    amountLayout->addWidget(amountTitle);

    auto* chipGrid = new QGridLayout();
    chipGrid->setSpacing(10);
    // 预设档位是 UI 选择；单笔上限已随契约 v1 §3 冻结（≤100000 元），
    // 校验走 WalletService::kMaximumRechargeCents 单一来源。
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
    customAmountEdit_->setPlaceholderText(tr("其他金额（元），最多 %1")
                                              .arg(formatCentsAsYuan(
                                                  WalletService::kMaximumRechargeCents)));
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

void RechargePage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updatePendingRechargeNotice(); // 进页即反映上次会话留下的未确认意图。
}

void RechargePage::updatePendingRechargeNotice()
{
    const qint64 pending = service_ != nullptr ? service_->pendingRechargeAmount() : 0;
    if (pending > 0) {
        pendingNoticeLabel_->setText(
            tr("有一笔 ¥%1 的充值结果未确认，重试沿用原流水号，不会重复入账")
                .arg(formatCentsAsYuan(pending)));
    }
    pendingBar_->setVisible(pending > 0);
}

void RechargePage::setSubmitting(bool submitting)
{
    confirmButton_->setEnabled(!submitting);
    backButton_->setEnabled(!submitting);
    customAmountEdit_->setEnabled(!submitting);
    if (retryButton_ != nullptr) {
        retryButton_->setEnabled(!submitting);
    }
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
    updatePendingRechargeNotice(); // 明确成功后意图已清理，撤下恢复条。
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
    // 明确失败（INVALID_ARGUMENT/IDEMPOTENCY_CONFLICT/RECHARGE_FAILED）会清掉
    // 持久化意图，恢复条随之撤下；超时/断线保留意图，条保持可见可重试。
    updatePendingRechargeNotice();
    Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);
}

} // namespace charging::client
