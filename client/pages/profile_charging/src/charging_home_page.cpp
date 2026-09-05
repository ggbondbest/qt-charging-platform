#include "charging/client/profile_charging/charging_home_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/charging_pulse.h"
#include "charging/client/profile_charging/order_status_display.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"
#include "charging/common/model/enums.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace charging::client {

namespace {

constexpr int kMaxRecentRows = 3;

QDateTime reservationStartLocal(
    const charging::client::services::reservation::ReservationRecord& record)
{
    return record.startAtUtc.toLocalTime();
}

} // namespace

ChargingHomePage::ChargingHomePage(
    ChargingService* chargingService, OrderService* orderService,
    charging::client::services::reservation::ReservationService* reservationService,
    QWidget* parent)
    : QWidget(parent), chargingService_(chargingService), orderService_(orderService),
      reservationService_(reservationService)
{
    buildUi();

    connect(orderService_, &OrderService::ordersLoaded, this, &ChargingHomePage::onOrdersLoaded);
    connect(orderService_, &OrderService::operationFailed, this,
            &ChargingHomePage::onOperationFailed);
    connect(chargingService_, &ChargingService::statusLoaded, this,
            &ChargingHomePage::onStatusLoaded);
    connect(chargingService_, &ChargingService::startCompleted, this,
            &ChargingHomePage::onStartCompleted);
    connect(chargingService_, &ChargingService::stopCompleted, this,
            &ChargingHomePage::onStopCompleted);
    connect(chargingService_, &ChargingService::paymentCompleted, this,
            &ChargingHomePage::onPaymentCompleted);
    connect(chargingService_, &ChargingService::operationFailed, this,
            &ChargingHomePage::onOperationFailed);
    connect(reservationService_,
            &charging::client::services::reservation::ReservationService::listSucceeded, this,
            &ChargingHomePage::onReservationsLoaded);
    connect(reservationService_,
            &charging::client::services::reservation::ReservationService::cancelSucceeded, this,
            [this](qint64) {
                Toast::show(this, tr("预约已取消"), StatusTag::Tone::Success);
                requestStates();
            });
    connect(reservationService_,
            &charging::client::services::reservation::ReservationService::cancelFailed, this,
            [this](const QString& message) {
                Toast::show(this, message, StatusTag::Tone::Danger);
            });

    countdownTimer_ = new QTimer(this);
    countdownTimer_->setInterval(1000);
    connect(countdownTimer_, &QTimer::timeout, this, &ChargingHomePage::tickCountdowns);
}

void ChargingHomePage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(14);

    auto* titleLabel = new QLabel(tr("充电"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    rootLayout->addWidget(titleLabel);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("uiRecordsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* container = new QWidget(scroll);
    cardsLayout_ = new QVBoxLayout(container);
    cardsLayout_->setContentsMargins(0, 0, 0, 0);
    cardsLayout_->setSpacing(12);
    cardsLayout_->addStretch();
    scroll->setWidget(container);
    rootLayout->addWidget(scroll, 1);
}

void ChargingHomePage::refresh()
{
    requestStates();
}

void ChargingHomePage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
    countdownTimer_->start();
}

void ChargingHomePage::hideEvent(QHideEvent* event)
{
    countdownTimer_->stop();
    chargingService_->stopTracking();
    QWidget::hideEvent(event);
}

void ChargingHomePage::requestStates()
{
    if (!orderService_->isFetchingOrders()) {
        orderService_->fetchOrders(OrderService::Filter::All, 1);
    }
    reservationService_->fetchList();
}

void ChargingHomePage::onOrdersLoaded(const QVector<charging::client::OrderSummary>& orders,
                                       int total, bool hasMore)
{
    Q_UNUSED(total);
    Q_UNUSED(hasMore);
    if (!isVisible()) {
        return;
    }
    chargingOrders_.clear();
    waitingOrders_.clear();
    recentDoneOrders_.clear();
    for (const charging::client::OrderSummary& summary : orders) {
        switch (summary.order.status) {
        case charging::model::OrderStatus::Charging:
            chargingOrders_.append(summary);
            break;
        case charging::model::OrderStatus::WaitingPayment:
            waitingOrders_.append(summary);
            break;
        case charging::model::OrderStatus::Completed:
            if (recentDoneOrders_.size() < kMaxRecentRows) {
                recentDoneOrders_.append(summary);
            }
            break;
        default:
            break;
        }
    }
    rebuildCards();
}

void ChargingHomePage::onReservationsLoaded(
    const charging::client::services::reservation::ReservationList& records)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    upcomingReservations_.clear();
    for (const auto& record : records) {
        if (record.reservation.status == charging::model::ReservationStatus::Active &&
            record.reservation.expiresAtUtc.isValid() &&
            record.reservation.expiresAtUtc > now && !record.lateCancelled &&
            upcomingReservations_.size() < 2) {
            upcomingReservations_.append(record);
        }
    }
    if (isVisible()) {
        rebuildCards();
    }
}

void ChargingHomePage::clearCards()
{
    while (cardsLayout_->count() > 1) {
        QLayoutItem* item = cardsLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    livePowerLabel_ = nullptr;
    liveEnergyLabel_ = nullptr;
    liveDurationLabel_ = nullptr;
    liveAmountLabel_ = nullptr;
    liveOrderId_ = 0;
    countdownLabels_.clear();
}

void ChargingHomePage::rebuildCards()
{
    clearCards();
    auto addCard = [this](QWidget* widget) {
        cardsLayout_->insertWidget(cardsLayout_->count() - 1, widget);
    };

    const bool hasCharging = !chargingOrders_.isEmpty();
    if (hasCharging) {
        addCard(buildChargingCard(chargingOrders_.first()));
    }
    for (const charging::client::OrderSummary& summary : waitingOrders_) {
        addCard(buildPaymentCard(summary));
    }
    if (!hasCharging) {
        for (const auto& record : upcomingReservations_) {
            addCard(buildReservationCard(record));
        }
    }
    if (!hasCharging && waitingOrders_.isEmpty() && upcomingReservations_.isEmpty()) {
        addCard(buildIdleView());
        for (const charging::client::OrderSummary& summary : recentDoneOrders_) {
            addCard(buildRecentRow(summary));
        }
    }

    if (hasCharging) {
        chargingService_->startTracking(chargingOrders_.first().order.id);
    } else {
        chargingService_->stopTracking();
    }
    tickCountdowns();
}

QWidget* ChargingHomePage::buildChargingCard(const charging::client::OrderSummary& summary)
{
    liveOrderId_ = summary.order.id;

    auto* card = new Card(this);
    auto* layout = card->bodyLayout();
    layout->setSpacing(6);

    auto* topRow = new QHBoxLayout();
    auto* stationLabel = new QLabel(summary.stationName.isEmpty() ? tr("未知充电站")
                                                                  : summary.stationName,
                                    card);
    stationLabel->setProperty("role", QStringLiteral("sectionTitle"));
    const OrderStatusDisplay display = orderStatusDisplay(summary.order.status);
    topRow->addWidget(stationLabel);
    topRow->addStretch();
    topRow->addWidget(new StatusTag(display.text, display.tone, card));
    layout->addLayout(topRow);

    auto* metaLabel = new QLabel(
        tr("桩号 %1").arg(summary.chargerCode.isEmpty() ? QStringLiteral("--")
                                                         : summary.chargerCode),
        card);
    metaLabel->setProperty("role", QStringLiteral("secondary"));
    layout->addWidget(metaLabel);

    auto* powerRow = new QHBoxLayout();
    livePowerLabel_ = new QLabel(QStringLiteral("--"), card);
    livePowerLabel_->setProperty("role", QStringLiteral("powerValue"));
    auto* unitLabel = new QLabel(QStringLiteral("kW"), card);
    unitLabel->setProperty("role", QStringLiteral("balanceUnit"));
    powerRow->addWidget(livePowerLabel_);
    powerRow->addWidget(unitLabel);
    powerRow->addStretch();
    auto* pulse = new ChargingPulse(card);
    pulse->setActive(true);
    powerRow->addWidget(pulse, 0, Qt::AlignVCenter);
    layout->addLayout(powerRow);

    auto* statsRow = new QHBoxLayout();
    statsRow->setSpacing(10);
    liveEnergyLabel_ = new QLabel(QStringLiteral("--"), card);
    liveEnergyLabel_->setProperty("role", QStringLiteral("statValue"));
    liveDurationLabel_ = new QLabel(QStringLiteral("--"), card);
    liveDurationLabel_->setProperty("role", QStringLiteral("statValue"));
    liveAmountLabel_ = new QLabel(QStringLiteral("--"), card);
    liveAmountLabel_->setProperty("role", QStringLiteral("statValue"));
    const struct
    {
        QLabel* value;
        QString caption;
    } stats[3] = {
        {liveEnergyLabel_, tr("已充电量 (kWh)")},
        {liveDurationLabel_, tr("充电时长")},
        {liveAmountLabel_, tr("预估费用 (元)")},
    };
    for (const auto& stat : stats) {
        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        column->addWidget(stat.value);
        auto* caption = new QLabel(stat.caption, card);
        caption->setProperty("role", QStringLiteral("caption"));
        column->addWidget(caption);
        statsRow->addLayout(column);
        statsRow->addStretch();
    }
    layout->addLayout(statsRow);

    auto* stopButton = new ActionButton(ActionButton::Variant::Danger, tr("停止充电"), card);
    connect(stopButton, &ActionButton::clicked, this, &ChargingHomePage::requestStop);
    layout->addWidget(stopButton);
    return card;
}

QWidget* ChargingHomePage::buildPaymentCard(const charging::client::OrderSummary& summary)
{
    auto* card = new Card(this);
    auto* layout = card->bodyLayout();
    layout->setSpacing(6);

    auto* topRow = new QHBoxLayout();
    auto* stationLabel = new QLabel(summary.stationName.isEmpty() ? tr("未知充电站")
                                                                  : summary.stationName,
                                    card);
    stationLabel->setProperty("role", QStringLiteral("sectionTitle"));
    const OrderStatusDisplay display = orderStatusDisplay(summary.order.status);
    topRow->addWidget(stationLabel);
    topRow->addStretch();
    topRow->addWidget(new StatusTag(display.text, display.tone, card));
    layout->addLayout(topRow);

    auto* breakdownLabel = new QLabel(
        tr("%1 kWh · %2 · 单价 ¥%3/kWh")
            .arg(formatEnergyWhAsKwh(summary.order.energyWh),
                 formatDurationHms(summary.order.durationSeconds),
                 formatCentsPerKwh(summary.order.unitPriceCentsPerKwh)),
        card);
    breakdownLabel->setProperty("role", QStringLiteral("secondary"));
    layout->addWidget(breakdownLabel);

    auto* bottomRow = new QHBoxLayout();
    auto* amountLabel =
        new QLabel(QStringLiteral("¥%1").arg(formatCentsAsYuan(summary.order.amountCents)), card);
    amountLabel->setProperty("role", QStringLiteral("amountStrong"));
    bottomRow->addWidget(amountLabel);
    bottomRow->addStretch();
    auto* payButton = new ActionButton(ActionButton::Variant::Primary, tr("去支付"), card);
    const qint64 orderId = summary.order.id;
    connect(payButton, &ActionButton::clicked, this, [this, orderId]() {
        if (chargingService_->isPaying()) {
            return;
        }
        chargingService_->payOrder(orderId);
    });
    bottomRow->addWidget(payButton);
    layout->addLayout(bottomRow);
    return card;
}

QWidget* ChargingHomePage::buildReservationCard(
    const charging::client::services::reservation::ReservationRecord& record)
{
    auto* card = new Card(this);
    auto* layout = card->bodyLayout();
    layout->setSpacing(6);

    auto* topRow = new QHBoxLayout();
    auto* stationLabel = new QLabel(record.stationName.isEmpty() ? tr("未知充电站")
                                                                 : record.stationName,
                                    card);
    stationLabel->setProperty("role", QStringLiteral("sectionTitle"));
    topRow->addWidget(stationLabel);
    topRow->addStretch();
    topRow->addWidget(new StatusTag(tr("已预约"), StatusTag::Tone::Info, card));
    layout->addLayout(topRow);

    const QString slotText =
        QStringLiteral("%1–%2")
            .arg(reservationStartLocal(record).toString(QStringLiteral("HH:mm")),
                 record.reservation.expiresAtUtc.toLocalTime().toString(QStringLiteral("HH:mm")));
    auto* metaLabel = new QLabel(tr("桩号 %1 · 时段 %2")
                                     .arg(record.chargerCode.isEmpty()
                                              ? QStringLiteral("--")
                                              : record.chargerCode,
                                          slotText),
                                 card);
    metaLabel->setProperty("role", QStringLiteral("secondary"));
    layout->addWidget(metaLabel);

    auto* countdownLabel = new QLabel(card);
    countdownLabel->setProperty("role", QStringLiteral("hintWarn"));
    layout->addWidget(countdownLabel);
    countdownLabels_.append({countdownLabel, record.startAtUtc});

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    auto* startButton = new ActionButton(ActionButton::Variant::Primary, tr("开始充电"), card);
    const qint64 reservationId = record.reservation.id;
    connect(startButton, &ActionButton::clicked, this, [this, reservationId]() {
        if (chargingService_->isStarting()) {
            return;
        }
        chargingService_->startCharging(reservationId);
    });
    auto* cancelButton = new ActionButton(ActionButton::Variant::Secondary, tr("取消"), card);
    connect(cancelButton, &ActionButton::clicked, this,
            [this, reservationId]() { reservationService_->cancel(reservationId); });
    buttonRow->addWidget(startButton);
    buttonRow->addWidget(cancelButton);
    layout->addLayout(buttonRow);
    return card;
}

QWidget* ChargingHomePage::buildIdleView()
{
    auto* hero = new QWidget(this);
    hero->setObjectName(QStringLiteral("uiChargingHero"));
    hero->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(hero);
    layout->setContentsMargins(24, 28, 24, 28);
    layout->setSpacing(6);

    auto* glyph = new QLabel(QStringLiteral("⚡"), hero);
    glyph->setObjectName(QStringLiteral("uiHeroGlyph"));
    auto* title = new QLabel(tr("当前没有充电任务"), hero);
    title->setObjectName(QStringLiteral("uiHeroTitle"));
    auto* caption = new QLabel(tr("找站或模拟扫码，开始一次充电"), hero);
    caption->setObjectName(QStringLiteral("uiHeroCaption"));
    layout->addWidget(glyph);
    layout->addWidget(title);
    layout->addWidget(caption);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    auto* findButton = new ActionButton(ActionButton::Variant::Primary, tr("去找站"), hero);
    connect(findButton, &ActionButton::clicked, this, [this]() { emit goFindStation(); });
    auto* scanButton = new ActionButton(ActionButton::Variant::Secondary, tr("模拟扫码"), hero);
    connect(scanButton, &ActionButton::clicked, this, [this]() {
        if (startingScan_ || chargingService_->isStarting()) {
            return;
        }
        startingScan_ = true;
        Toast::show(this, tr("模拟扫码成功，正在启动充电…"), StatusTag::Tone::Success);
        // reservationId 0 = walk-up scan（服务端尚未定义，TODO(contract)；mock 接受）。
        chargingService_->startCharging(0);
    });
    buttonRow->addWidget(findButton);
    buttonRow->addWidget(scanButton);
    layout->addSpacing(10);
    layout->addLayout(buttonRow);
    return hero;
}

QWidget* ChargingHomePage::buildRecentRow(const charging::client::OrderSummary& summary)
{
    auto* row = new ClickableCard(this);
    auto* layout = row->bodyLayout();
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(4);

    auto* topRow = new QHBoxLayout();
    auto* stationLabel = new QLabel(summary.stationName.isEmpty() ? tr("未知充电站")
                                                                  : summary.stationName,
                                    row);
    stationLabel->setProperty("role", QStringLiteral("sectionTitle"));
    topRow->addWidget(stationLabel);
    topRow->addStretch();
    topRow->addWidget(new StatusTag(tr("已完成"), StatusTag::Tone::Neutral, row));
    layout->addLayout(topRow);

    const QDateTime displayTime = summary.order.startedAtUtc.isValid()
                                      ? summary.order.startedAtUtc
                                      : summary.order.createdAtUtc;
    auto* bottomRow = new QHBoxLayout();
    auto* metaLabel = new QLabel(formatDateTimeLocal(displayTime), row);
    metaLabel->setProperty("role", QStringLiteral("caption"));
    auto* amountLabel =
        new QLabel(QStringLiteral("¥%1").arg(formatCentsAsYuan(summary.order.amountCents)), row);
    amountLabel->setProperty("role", QStringLiteral("amountStrong"));
    bottomRow->addWidget(metaLabel);
    bottomRow->addStretch();
    bottomRow->addWidget(amountLabel);
    layout->addLayout(bottomRow);

    const charging::client::OrderSummary captured = summary;
    connect(row, &ClickableCard::clicked, this,
            [this, captured]() { emit orderOpened(captured); });
    return row;
}

void ChargingHomePage::onStatusLoaded(const charging::client::ChargingStatus& status)
{
    if (status.order.id != liveOrderId_ || livePowerLabel_ == nullptr) {
        return;
    }
    livePowerLabel_->setText(status.powerKnown ? formatWattsAsKw(status.powerWatts)
                                               : QStringLiteral("--"));
    liveEnergyLabel_->setText(formatEnergyWhAsKwh(status.order.energyWh));
    liveDurationLabel_->setText(formatDurationHms(status.order.durationSeconds));
    liveAmountLabel_->setText(formatCentsAsYuan(status.order.amountCents));
}

void ChargingHomePage::onStartCompleted(const charging::client::ChargingStatus& status)
{
    startingScan_ = false;
    Q_UNUSED(status);
    Toast::show(this, tr("充电已启动"), StatusTag::Tone::Success);
    requestStates();
}

void ChargingHomePage::onStopCompleted(const charging::client::ChargingStatus& status)
{
    emit settlementRequested(status);
    requestStates();
}

void ChargingHomePage::onPaymentCompleted(qint64 amountCents, qint64 balanceAfterCents)
{
    Q_UNUSED(amountCents);
    Toast::show(this, tr("支付成功，余额 ¥%1").arg(formatCentsAsYuan(balanceAfterCents)),
                StatusTag::Tone::Success);
    requestStates();
}

void ChargingHomePage::onOperationFailed(const QString& type,
                                         const charging::protocol::ProtocolError& error)
{
    const QString startType =
        QString::fromLatin1(charging::protocol::request_type::kStartCharging);
    const QString stopType = QString::fromLatin1(charging::protocol::request_type::kStopCharging);
    const QString payType = QString::fromLatin1(charging::protocol::request_type::kPayOrder);
    if (type == startType || type == stopType || type == payType) {
        if (type == startType) {
            startingScan_ = false;
        }
        Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);
    }
}

void ChargingHomePage::requestStop()
{
    if (chargingService_->isStoppingCharging()) {
        return;
    }
    QMessageBox box(QMessageBox::Question, tr("确认停止充电"),
                    tr("确定停止充电吗？\n结束后按实际电量与时长结算。"), QMessageBox::NoButton,
                    this);
    QPushButton* confirm = box.addButton(tr("确认停止"), QMessageBox::AcceptRole);
    box.addButton(tr("继续充电"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != confirm) {
        return;
    }
    chargingService_->stopCharging();
}

void ChargingHomePage::tickCountdowns()
{
    for (const auto& entry : countdownLabels_) {
        const qint64 secs = QDateTime::currentDateTime().secsTo(entry.second);
        if (secs > 0) {
            entry.first->setText(tr("距开始 %1").arg(formatDurationHms(secs)));
        } else {
            entry.first->setText(tr("时段进行中，请尽快连接启动"));
        }
    }
}

} // namespace charging::client
