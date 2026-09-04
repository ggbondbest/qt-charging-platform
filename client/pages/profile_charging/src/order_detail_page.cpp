#include "charging/client/profile_charging/order_detail_page.h"

#include "charging/client/profile_charging/order_status_display.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_bar.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/common/model/enums.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace charging::client {

namespace {

QString textOrPlaceholder(const QString& text)
{
    return text.isEmpty() ? QStringLiteral("--") : text;
}

QString timeOrPlaceholder(const QDateTime& utcValue)
{
    return utcValue.isValid() ? formatDateTimeLocal(utcValue) : QStringLiteral("--");
}

} // namespace

OrderDetailPage::OrderDetailPage(QWidget* parent) : QWidget(parent)
{
    buildUi();
}

void OrderDetailPage::buildUi()
{
    // 外框：内容区 + 底部操作条（去支付钉在页面底部，与充值/结算一致）。
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);
    auto* content = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(20, 16, 20, 16);
    rootLayout->setSpacing(14);
    outerLayout->addWidget(content, 1);

    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("订单详情"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    backButton_ = new ActionButton(ActionButton::Variant::Ghost, tr("返回"), this);
    connect(backButton_, &ActionButton::clicked, this, &OrderDetailPage::backRequested);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    headerRow->addWidget(backButton_);
    rootLayout->addLayout(headerRow);

    summaryCard_ = new Card(this);
    auto* summaryLayout = summaryCard_->bodyLayout();
    summaryLayout->setSpacing(6);

    auto* topRow = new QHBoxLayout();
    stationLabel_ = new QLabel(QStringLiteral("--"), summaryCard_);
    stationLabel_->setProperty("role", QStringLiteral("sectionTitle"));
    statusTag_ = new StatusTag(QString(), StatusTag::Tone::Neutral, summaryCard_);
    topRow->addWidget(stationLabel_);
    topRow->addStretch();
    topRow->addWidget(statusTag_);
    summaryLayout->addLayout(topRow);

    metaLabel_ = new QLabel(QStringLiteral("--"), summaryCard_);
    metaLabel_->setProperty("role", QStringLiteral("secondary"));
    summaryLayout->addWidget(metaLabel_);

    amountLabel_ = new QLabel(QStringLiteral("¥ --"), summaryCard_);
    // 订单金额是信息而非余额横幅，降级为行内强金额，避免 34px 数字满屏轰炸。
    amountLabel_->setProperty("role", QStringLiteral("amountStrong"));
    summaryLayout->addWidget(amountLabel_);

    auto* usageLine = new QHBoxLayout();
    energyLabel_ = new QLabel(QStringLiteral("--"), summaryCard_);
    energyLabel_->setProperty("role", QStringLiteral("caption"));
    durationLabel_ = new QLabel(QStringLiteral("--"), summaryCard_);
    durationLabel_->setProperty("role", QStringLiteral("caption"));
    usageLine->addWidget(energyLabel_);
    usageLine->addStretch();
    usageLine->addWidget(durationLabel_);
    summaryLayout->addLayout(usageLine);

    rootLayout->addWidget(summaryCard_);

    auto* detailCard = new Card(this);
    detailRowsLayout_ = detailCard->bodyLayout();
    detailRowsLayout_->setSpacing(10);
    rootLayout->addWidget(detailCard);
    rootLayout->addStretch();

    payBar_ = new ActionBar(ActionBar::Variant::Primary, tr("去支付"), this);
    payBar_->setVisible(false);
    outerLayout->addWidget(payBar_);
    payButton_ = payBar_->actionButton();
    connect(payButton_, &ActionButton::clicked, this, &OrderDetailPage::payRequested);
}

void OrderDetailPage::setEmbedded(bool embedded)
{
    // 全局顶部导航已提供返回，隐藏页内返回按钮。
    backButton_->setVisible(!embedded);
}

void OrderDetailPage::addInfoRow(QVBoxLayout* layout, const QString& label, const QString& value)
{
    auto* row = new QHBoxLayout();
    auto* keyLabel = new QLabel(label, this);
    keyLabel->setProperty("role", QStringLiteral("secondary"));
    auto* valueLabel = new QLabel(value, this);
    valueLabel->setProperty("role", QStringLiteral("infoValue"));
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(keyLabel);
    row->addStretch();
    row->addWidget(valueLabel);
    layout->addLayout(row);
}

void OrderDetailPage::showOrder(const charging::client::OrderSummary& summary)
{
    summary_ = summary;
    const charging::model::Order& order = summary_.order;

    stationLabel_->setText(textOrPlaceholder(summary_.stationName));
    const QDateTime displayTime =
        order.startedAtUtc.isValid() ? order.startedAtUtc : order.createdAtUtc;
    metaLabel_->setText(tr("桩号 %1 · %2").arg(textOrPlaceholder(summary_.chargerCode),
                                               timeOrPlaceholder(displayTime)));

    const OrderStatusDisplay statusDisplay = orderStatusDisplay(order.status);
    statusTag_->setText(statusDisplay.text);
    statusTag_->setTone(statusDisplay.tone);

    amountLabel_->setText(QStringLiteral("¥%1").arg(formatCentsAsYuan(order.amountCents)));
    energyLabel_->setText(tr("已充电量 %1 kWh").arg(formatEnergyWhAsKwh(order.energyWh)));
    durationLabel_->setText(tr("时长 %1").arg(formatDurationHms(order.durationSeconds)));

    // Rebuild the fixed key/value rows.
    while (detailRowsLayout_->count() > 0) {
        QLayoutItem* item = detailRowsLayout_->takeAt(0);
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
    addInfoRow(detailRowsLayout_, tr("订单编号"), order.orderNo);
    addInfoRow(detailRowsLayout_, tr("开始时间"), timeOrPlaceholder(order.startedAtUtc));
    addInfoRow(detailRowsLayout_, tr("结束时间"), timeOrPlaceholder(order.stoppedAtUtc));
    addInfoRow(detailRowsLayout_, tr("充电时长"), formatDurationHms(order.durationSeconds));
    addInfoRow(detailRowsLayout_, tr("充电电量"),
               tr("%1 kWh").arg(formatEnergyWhAsKwh(order.energyWh)));
    addInfoRow(detailRowsLayout_, tr("单价"),
               tr("%1 元/度").arg(formatCentsPerKwh(order.unitPriceCentsPerKwh)));
    addInfoRow(detailRowsLayout_, tr("支付时间"), timeOrPlaceholder(order.paidAtUtc));

    payBar_->setVisible(order.status == charging::model::OrderStatus::WaitingPayment);
}

} // namespace charging::client
