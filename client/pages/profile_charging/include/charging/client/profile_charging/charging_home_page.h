#pragma once

#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/order_service.h"
#include "services/reservation/reservation_service.h"

#include <QDateTime>
#include <QPair>
#include <QVector>
#include <QWidget>

class QLabel;
class QTimer;
class QVBoxLayout;

namespace charging::client {

class ActionButton;

// 「充电」Tab 根页：按充电生命周期状态堆叠卡片——
//   充电中卡（功率/电量/时长/停止）→ 待支付卡（费用明细/支付）→
//   预约卡（倒计时/开始充电/取消，充电中时不显示）→ 全无时显示
//   无任务引导（去找站 / 模拟扫码 + 最近充过）。
// 钱包不在此页（已降为「我的」入口下的路由页）。
class ChargingHomePage final : public QWidget
{
    Q_OBJECT

public:
    ChargingHomePage(ChargingService* chargingService, OrderService* orderService,
                     charging::client::services::reservation::ReservationService* reservationService,
                     QWidget* parent = nullptr);

    // 重查订单与预约列表并重建卡片（切 Tab 进入时由壳层调用）。
    void refresh();

    // 「模拟扫码」只是 mock 预览通道能力：契约 v1 §4 明确把扫码动作排除在
    // 协议外，真实模式下 reservationId 0 必被服务端拒绝，入口应隐藏而不是
    // 假装扫码成功（壳层在有 connection 时调用 setScanDemoAvailable(false)）。
    void setScanDemoAvailable(bool available);

signals:
    void goFindStation(); // 壳层切到「找站」Tab
    void orderOpened(const charging::client::OrderSummary& summary);
    void settlementRequested(const charging::client::ChargingStatus& status);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void requestStates();
    void rebuildCards();
    void clearCards();

    QWidget* buildChargingCard(const charging::client::OrderSummary& summary);
    QWidget* buildPaymentCard(const charging::client::OrderSummary& summary);
    QWidget* buildReservationCard(
        const charging::client::services::reservation::ReservationRecord& record);
    QWidget* buildIdleView();
    QWidget* buildRecentRow(const charging::client::OrderSummary& summary);

    void onOrdersLoaded(const QVector<charging::client::OrderSummary>& orders, int total,
                        bool hasMore);
    void onReservationsLoaded(
        const charging::client::services::reservation::ReservationList& records);
    void onOperationFailed(const QString& type,
                           const charging::protocol::ProtocolError& error);
    void onStartCompleted(const charging::client::ChargingStatus& status);
    void onStopCompleted(const charging::client::ChargingStatus& status);
    void onPaymentCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void onStatusLoaded(const charging::client::ChargingStatus& status);
    void tickCountdowns();
    void requestStop();

    ChargingService* chargingService_ = nullptr;
    OrderService* orderService_ = nullptr;
    charging::client::services::reservation::ReservationService* reservationService_ = nullptr;

    QVBoxLayout* cardsLayout_ = nullptr;
    QTimer* countdownTimer_ = nullptr;

    // 最近一次订单列表的分类结果（服务端仍是唯一事实来源）。
    QVector<charging::client::OrderSummary> chargingOrders_;
    QVector<charging::client::OrderSummary> waitingOrders_;
    QVector<charging::client::OrderSummary> recentDoneOrders_;
    charging::client::services::reservation::ReservationList upcomingReservations_;

    // 充电中卡的实时标签（statusLoaded 每秒刷新）。
    QLabel* livePowerLabel_ = nullptr;
    QLabel* liveEnergyLabel_ = nullptr;
    QLabel* liveDurationLabel_ = nullptr;
    QLabel* liveAmountLabel_ = nullptr;
    qint64 liveOrderId_ = 0;

    // 预约卡倒计时标签（每秒 tick 重写文本）。
    QVector<QPair<QLabel*, QDateTime>> countdownLabels_;
    bool startingScan_ = false;
    ActionButton* scanButton_ = nullptr;
    QLabel* heroCaptionLabel_ = nullptr;
    bool scanDemoAvailable_ = true;
};

} // namespace charging::client
