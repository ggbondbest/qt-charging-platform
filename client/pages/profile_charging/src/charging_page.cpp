#include "charging/client/profile_charging/charging_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/order_status_display.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_bar.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/loading_overlay.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace charging::client {

ChargingPage::ChargingPage(ChargingService* service, QWidget* parent)
    : QWidget(parent), service_(service)
{
    buildUi();

    connect(service_, &ChargingService::statusLoaded, this, &ChargingPage::onStatusLoaded);
    connect(service_, &ChargingService::stopCompleted, this, &ChargingPage::onStopCompleted);
    connect(service_, &ChargingService::operationFailed, this, &ChargingPage::onOperationFailed);
}

void ChargingPage::buildUi()
{
    // 外框：内容区 + 底部操作条（停止充电钉底，危险操作不随内容漂移）。
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);
    auto* content = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(20, 16, 20, 16);
    rootLayout->setSpacing(14);
    outerLayout->addWidget(content, 1);

    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("充电过程"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    backButton_ = new ActionButton(ActionButton::Variant::Ghost, tr("返回"), this);
    connect(backButton_, &ActionButton::clicked, this, &ChargingPage::backRequested);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    headerRow->addWidget(backButton_);
    rootLayout->addLayout(headerRow);

    heroCard_ = new Card(this);
    auto* heroLayout = heroCard_->bodyLayout();
    heroLayout->setSpacing(6);

    auto* topRow = new QHBoxLayout();
    stationLabel_ = new QLabel(QStringLiteral("--"), heroCard_);
    stationLabel_->setProperty("role", QStringLiteral("sectionTitle"));
    statusTag_ = new StatusTag(QString(), StatusTag::Tone::Success, heroCard_);
    topRow->addWidget(stationLabel_);
    topRow->addStretch();
    topRow->addWidget(statusTag_);
    heroLayout->addLayout(topRow);

    metaLabel_ = new QLabel(QStringLiteral("--"), heroCard_);
    metaLabel_->setProperty("role", QStringLiteral("secondary"));
    heroLayout->addWidget(metaLabel_);

    auto* powerRow = new QHBoxLayout();
    powerValueLabel_ = new QLabel(QStringLiteral("--"), heroCard_);
    powerValueLabel_->setProperty("role", QStringLiteral("powerValue"));
    auto* powerUnitLabel = new QLabel(QStringLiteral("kW"), heroCard_);
    powerUnitLabel->setProperty("role", QStringLiteral("balanceUnit"));
    powerRow->addWidget(powerValueLabel_);
    powerRow->addWidget(powerUnitLabel);
    powerRow->addStretch();
    heroLayout->addLayout(powerRow);

    powerCaptionLabel_ = new QLabel(tr("实时功率"), heroCard_);
    powerCaptionLabel_->setProperty("role", QStringLiteral("caption"));
    heroLayout->addWidget(powerCaptionLabel_);

    auto* statsRow = new QHBoxLayout();
    statsRow->setSpacing(10);
    const struct
    {
        QLabel** value;
        QString labelText;
    } stats[3] = {
        {&energyValueLabel_, tr("已充电量 (kWh)")},
        {&durationValueLabel_, tr("充电时长")},
        {&estimateValueLabel_, tr("预估费用 (元)")},
    };
    for (const auto& stat : stats) {
        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        auto* valueLabel = new QLabel(QStringLiteral("--"), heroCard_);
        valueLabel->setProperty("role", QStringLiteral("statValue"));
        auto* nameLabel = new QLabel(stat.labelText, heroCard_);
        nameLabel->setProperty("role", QStringLiteral("caption"));
        column->addWidget(valueLabel);
        column->addWidget(nameLabel);
        *stat.value = valueLabel;
        statsRow->addLayout(column);
        statsRow->addStretch();
    }
    heroLayout->addLayout(statsRow);

    updatedLabel_ = new QLabel(QString(), heroCard_);
    updatedLabel_->setProperty("role", QStringLiteral("caption"));
    updatedLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    heroLayout->addWidget(updatedLabel_);

    statusNotice_ = new NoticePanel(QStringLiteral("⚠"), tr("实时数据获取失败"), QString(),
                                    tr("重试"), this);
    statusNotice_->setVisible(false);
    connect(statusNotice_, &NoticePanel::actionTriggered, this,
            [this]() { service_->fetchStatusNow(); });

    rootLayout->addWidget(heroCard_);
    rootLayout->addWidget(statusNotice_);
    rootLayout->addStretch();

    stopBar_ = new ActionBar(ActionBar::Variant::Danger, tr("停止充电"), this);
    stopBar_->setCaption(tr("结束后按实际电量与时长结算"));
    outerLayout->addWidget(stopBar_);
    stopButton_ = stopBar_->actionButton();
    connect(stopButton_, &ActionButton::clicked, this, &ChargingPage::requestStop);

    overlay_ = new LoadingOverlay(this);
    overlay_->setVisible(false);
}

void ChargingPage::setEmbedded(bool embedded)
{
    // 全局顶部导航已提供返回，隐藏页内返回按钮。
    backButton_->setVisible(!embedded);
}

void ChargingPage::startFor(const charging::client::ChargingStatus& initial)
{
    latest_ = initial;
    hasData_ = false;
    stopButton_->setEnabled(true);
    statusNotice_->setVisible(false);
    render(initial);
    overlay_->showFor();
    service_->startTracking(initial.order.id);
}

void ChargingPage::render(const charging::client::ChargingStatus& status)
{
    stationLabel_->setText(status.stationName.isEmpty() ? tr("未知充电站") : status.stationName);
    const QDateTime displayTime = status.order.startedAtUtc.isValid()
                                      ? status.order.startedAtUtc
                                      : status.order.createdAtUtc;
    metaLabel_->setText(tr("桩号 %1 · %2 开始")
                            .arg(status.chargerCode.isEmpty() ? QStringLiteral("--")
                                                               : status.chargerCode,
                                 formatDateTimeLocal(displayTime)));

    const OrderStatusDisplay statusDisplay = orderStatusDisplay(status.order.status);
    statusTag_->setText(statusDisplay.text);
    statusTag_->setTone(statusDisplay.tone);

    if (status.powerKnown) {
        powerValueLabel_->setText(formatWattsAsKw(status.powerWatts));
        powerCaptionLabel_->setText(tr("实时功率"));
    } else {
        // Degrade honestly: never render a locally guessed power value.
        powerValueLabel_->setText(QStringLiteral("--"));
        powerCaptionLabel_->setText(tr("实时功率（本帧未上报）"));
    }

    energyValueLabel_->setText(formatEnergyWhAsKwh(status.order.energyWh));
    durationValueLabel_->setText(formatDurationHms(status.order.durationSeconds));
    // §8.4: the live charge for a CHARGING order is order.amountCents.
    estimateValueLabel_->setText(formatCentsAsYuan(status.order.amountCents));
    updatedLabel_->setText(
        tr("更新于 %1")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
}

void ChargingPage::onStatusLoaded(const charging::client::ChargingStatus& status)
{
    hasData_ = true;
    latest_ = status;
    render(status);
    overlay_->hideFor();
    statusNotice_->setVisible(false);
}

void ChargingPage::onStopCompleted(const charging::client::ChargingStatus& status)
{
    overlay_->hideFor();
    setStopBusy(false);
    emit settlementRequested(status);
}

void ChargingPage::setStopBusy(bool busy)
{
    stopButton_->setEnabled(!busy);
    if (busy) {
        overlay_->showFor();
    } else {
        overlay_->hideFor();
    }
}

void ChargingPage::requestStop()
{
    QMessageBox box(QMessageBox::Question, tr("确认停止充电"),
                    tr("当前已充电 %1 kWh，预估费用 ¥%2。\n确定停止充电吗？")
                        .arg(formatEnergyWhAsKwh(latest_.order.energyWh),
                             formatCentsAsYuan(latest_.order.amountCents)),
                    QMessageBox::NoButton, this);
    QPushButton* confirm = box.addButton(tr("确认停止"), QMessageBox::AcceptRole);
    box.addButton(tr("继续充电"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != confirm) {
        return;
    }

    setStopBusy(true);
    service_->stopCharging();
}

void ChargingPage::onOperationFailed(const QString& type,
                                     const charging::protocol::ProtocolError& error)
{
    const QString stopType = QString::fromLatin1(charging::protocol::request_type::kStopCharging);
    if (type == stopType) {
        setStopBusy(false);
        Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);
        return;
    }

    const QString statusType =
        QString::fromLatin1(charging::protocol::request_type::kGetChargingStatus);
    if (type == statusType) {
        overlay_->hideFor();
        if (!hasData_) {
            statusNotice_->setContent(QStringLiteral("⚠"), tr("实时数据获取失败"),
                                      displayMessageForError(error), tr("重试"));
            statusNotice_->setVisible(true);
        } else {
            // Keep the last good numbers on screen; say it quietly instead
            // of toasting on every failed poll tick.
            updatedLabel_->setText(tr("连接不稳定，正在重试…"));
        }
    }
}

} // namespace charging::client
