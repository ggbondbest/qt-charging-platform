#include "charging/client/profile_charging/settlement_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/loading_overlay.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"
#include "charging/common/protocol/protocol.h"

#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>

namespace charging::client {

SettlementPage::SettlementPage(ChargingService* service, QWidget* parent)
    : QWidget(parent), service_(service)
{
    buildUi();

    connect(service_, &ChargingService::paymentCompleted, this,
            &SettlementPage::onPaymentCompleted);
    connect(service_, &ChargingService::operationFailed, this,
            &SettlementPage::onOperationFailed);
}

void SettlementPage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(14);

    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("订单结算"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    auto* backButton = new ActionButton(ActionButton::Variant::Ghost, tr("返回"), this);
    connect(backButton, &ActionButton::clicked, this, &SettlementPage::backRequested);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    headerRow->addWidget(backButton);
    rootLayout->addLayout(headerRow);

    // ---------- pending state ----------
    pendingCard_ = new Card(this);
    auto* pendingLayout = pendingCard_->bodyLayout();
    pendingLayout->setSpacing(6);

    stationLabel_ = new QLabel(QStringLiteral("--"), pendingCard_);
    stationLabel_->setProperty("role", QStringLiteral("sectionTitle"));
    pendingLayout->addWidget(stationLabel_);

    amountLabel_ = new QLabel(QStringLiteral("¥ --"), pendingCard_);
    amountLabel_->setProperty("role", QStringLiteral("balance"));
    pendingLayout->addWidget(amountLabel_);

    auto* amountCaption = new QLabel(tr("本次充电费用"), pendingCard_);
    amountCaption->setProperty("role", QStringLiteral("caption"));
    pendingLayout->addWidget(amountCaption);

    infoRowsLayout_ = new QVBoxLayout();
    infoRowsLayout_->setSpacing(8);
    pendingLayout->addLayout(infoRowsLayout_);

    balanceLabel_ = new QLabel(QString(), pendingCard_);
    balanceLabel_->setProperty("role", QStringLiteral("secondary"));
    pendingLayout->addWidget(balanceLabel_);

    hintLabel_ = new QLabel(QString(), pendingCard_);
    hintLabel_->setProperty("role", QStringLiteral("hintWarn"));
    hintLabel_->setVisible(false);
    pendingLayout->addWidget(hintLabel_);

    rechargeButton_ = new ActionButton(ActionButton::Variant::Secondary, tr("去充值"),
                                       pendingCard_);
    rechargeButton_->setVisible(false);
    connect(rechargeButton_, &ActionButton::clicked, this, &SettlementPage::rechargeRequested);
    pendingLayout->addWidget(rechargeButton_);
    rootLayout->addWidget(pendingCard_);

    payButton_ = new ActionButton(ActionButton::Variant::Primary, tr("确认支付"), this);
    payButton_->setMinimumHeight(46);
    connect(payButton_, &ActionButton::clicked, this, &SettlementPage::requestPay);
    rootLayout->addWidget(payButton_);

    // ---------- paid result state ----------
    paidCard_ = new Card(this);
    auto* paidLayout = paidCard_->bodyLayout();
    paidLayout->setSpacing(6);
    auto* checkLabel = new QLabel(QStringLiteral("✓"), paidCard_);
    checkLabel->setProperty("role", QStringLiteral("successCheck"));
    checkLabel->setAlignment(Qt::AlignHCenter);
    paidLayout->addWidget(checkLabel);
    auto* paidTitle = new QLabel(tr("支付成功"), paidCard_);
    paidTitle->setProperty("role", QStringLiteral("sectionTitle"));
    paidTitle->setAlignment(Qt::AlignHCenter);
    paidLayout->addWidget(paidTitle);
    paidAmountLabel_ = new QLabel(QStringLiteral("¥ --"), paidCard_);
    paidAmountLabel_->setProperty("role", QStringLiteral("balance"));
    paidAmountLabel_->setAlignment(Qt::AlignHCenter);
    paidLayout->addWidget(paidAmountLabel_);
    paidBalanceLabel_ = new QLabel(QString(), paidCard_);
    paidBalanceLabel_->setProperty("role", QStringLiteral("caption"));
    paidBalanceLabel_->setAlignment(Qt::AlignHCenter);
    paidLayout->addWidget(paidBalanceLabel_);
    paidCard_->setVisible(false);
    rootLayout->addWidget(paidCard_);

    rootLayout->addStretch();

    doneButton_ = new ActionButton(ActionButton::Variant::Primary, tr("查看订单"), this);
    doneButton_->setMinimumHeight(46);
    doneButton_->setVisible(false);
    connect(doneButton_, &ActionButton::clicked, this, &SettlementPage::doneRequested);
    rootLayout->addWidget(doneButton_);

    overlay_ = new LoadingOverlay(this);
    overlay_->setVisible(false);
}

void SettlementPage::addInfoRow(QVBoxLayout* layout, const QString& label, const QString& value)
{
    auto* row = new QHBoxLayout();
    auto* keyLabel = new QLabel(label, pendingCard_);
    keyLabel->setProperty("role", QStringLiteral("secondary"));
    auto* valueLabel = new QLabel(value, pendingCard_);
    valueLabel->setProperty("role", QStringLiteral("infoValue"));
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(keyLabel);
    row->addStretch();
    row->addWidget(valueLabel);
    layout->addLayout(row);
}

void SettlementPage::showOrder(const charging::client::ChargingStatus& stopped)
{
    stopped_ = stopped;
    paidShown_ = false;
    renderPending();
}

void SettlementPage::setBalance(qint64 balanceCents)
{
    balanceCents_ = balanceCents;
    refreshAffordability();
}

void SettlementPage::renderPending()
{
    pendingCard_->setVisible(true);
    paidCard_->setVisible(false);
    payButton_->setVisible(true);
    doneButton_->setVisible(false);
    setPaying(false);

    const charging::model::Order& order = stopped_.order;
    QString station = stopped_.stationName.isEmpty() ? tr("充电站") : stopped_.stationName;
    if (!stopped_.chargerCode.isEmpty()) {
        station += tr(" · %1").arg(stopped_.chargerCode);
    }
    stationLabel_->setText(station);
    amountLabel_->setText(QStringLiteral("¥%1").arg(formatCentsAsYuan(order.amountCents)));
    payButton_->setText(tr("确认支付 ¥%1").arg(formatCentsAsYuan(order.amountCents)));

    while (infoRowsLayout_->count() > 0) {
        QLayoutItem* item = infoRowsLayout_->takeAt(0);
        if (item->layout() != nullptr) {
            QLayoutItem* child = nullptr;
            while ((child = item->layout()->takeAt(0)) != nullptr) {
                if (child->widget() != nullptr) {
                    child->widget()->deleteLater();
                }
                delete child;
            }
        }
        delete item;
    }
    addInfoRow(infoRowsLayout_, tr("订单编号"), order.orderNo);
    addInfoRow(infoRowsLayout_, tr("充电电量"),
               tr("%1 kWh").arg(formatEnergyWhAsKwh(order.energyWh)));
    addInfoRow(infoRowsLayout_, tr("充电时长"), formatDurationHms(order.durationSeconds));
    addInfoRow(infoRowsLayout_, tr("单价"),
               tr("%1 元/度").arg(formatCentsPerKwh(order.unitPriceCentsPerKwh)));

    refreshAffordability();
}

void SettlementPage::refreshAffordability()
{
    if (paidShown_) {
        return;
    }
    const qint64 amount = stopped_.order.amountCents;
    if (balanceCents_ < 0) {
        // Unknown balance: don't lock the user out; the server re-validates.
        balanceLabel_->setText(tr("当前余额 --"));
        hintLabel_->setVisible(false);
        rechargeButton_->setVisible(false);
        payButton_->setEnabled(true);
        return;
    }
    balanceLabel_->setText(tr("当前余额 ¥%1").arg(formatCentsAsYuan(balanceCents_)));
    const bool affordable = balanceCents_ >= amount;
    hintLabel_->setVisible(!affordable);
    hintLabel_->setText(tr("余额不足，请先充值"));
    rechargeButton_->setVisible(!affordable);
    payButton_->setEnabled(affordable); // UI guard only; server decides.
}

void SettlementPage::renderPaid(qint64 amountCents, qint64 balanceAfterCents)
{
    paidShown_ = true;
    pendingCard_->setVisible(false);
    payButton_->setVisible(false);
    paidCard_->setVisible(true);
    paidAmountLabel_->setText(QStringLiteral("¥%1").arg(formatCentsAsYuan(amountCents)));
    paidBalanceLabel_->setText(
        tr("支付后余额 ¥%1").arg(formatCentsAsYuan(balanceAfterCents)));
    doneButton_->setVisible(true);
}

void SettlementPage::requestPay()
{
    const qint64 amount = stopped_.order.amountCents;
    QMessageBox box(QMessageBox::Question, tr("确认支付"),
                    tr("将从钱包余额支付 ¥%1？").arg(formatCentsAsYuan(amount)),
                    QMessageBox::NoButton, this);
    QPushButton* confirm = box.addButton(tr("确认支付"), QMessageBox::AcceptRole);
    box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != confirm) {
        return;
    }

    setPaying(true);
    service_->payOrder(stopped_.order.id);
}

void SettlementPage::setPaying(bool busy)
{
    payButton_->setEnabled(!busy);
    if (busy) {
        overlay_->showFor();
    } else {
        overlay_->hideFor();
    }
}

void SettlementPage::onPaymentCompleted(qint64 amountCents, qint64 balanceAfterCents)
{
    setPaying(false);
    balanceCents_ = balanceAfterCents;
    renderPaid(amountCents, balanceAfterCents);
}

void SettlementPage::onOperationFailed(const QString& type,
                                       const charging::protocol::ProtocolError& error)
{
    const QString payType = QString::fromLatin1(charging::protocol::request_type::kPayOrder);
    if (type != payType) {
        return; // Status/stop failures belong to the charging page.
    }
    setPaying(false);
    Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);

    if (error.code == QString::fromLatin1(charging::protocol::error_code::kInsufficientBalance)) {
        // Pick up the authoritative balance from the error details, if sent.
        const QJsonValue balanceValue = error.details.value(QStringLiteral("balanceCents"));
        if (!balanceValue.isUndefined()) {
            balanceCents_ = static_cast<qint64>(balanceValue.toDouble(-1));
        }
        refreshAffordability();
    }
}

} // namespace charging::client
