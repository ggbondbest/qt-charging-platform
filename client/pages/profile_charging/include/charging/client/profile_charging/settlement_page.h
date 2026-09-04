#pragma once

#include "charging/client/profile_charging/charging_service.h"
#include "charging/common/protocol/protocol.h"

#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace charging::client {

class ActionBar;
class ActionButton;
class Card;
class LoadingOverlay;

// Settlement page shown after the server confirms a session stopped.
// The amount is whatever the server settled; this page only asks the server
// to pay (PAY_ORDER) and renders the result. The balance check on the pay
// button is a UI guard, not the business rule — the server re-validates.
class SettlementPage final : public QWidget
{
    Q_OBJECT

public:
    explicit SettlementPage(ChargingService* service, QWidget* parent = nullptr);

    // Fill the page with the stopped order; resets to the "pending" state.
    void showOrder(const charging::client::ChargingStatus& stopped);
    // Current wallet balance for the pre-pay hint (cents).
    void setBalance(qint64 balanceCents);
    // 整合壳层（HomeShell）内使用：隐藏页内返回按钮（全局顶部导航负责返回）。
    void setEmbedded(bool embedded);

signals:
    void backRequested();
    void rechargeRequested();
    void doneRequested(); // e.g. "查看订单" after successful payment

private slots:
    void onPaymentCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    void addInfoRow(QVBoxLayout* layout, const QString& label, const QString& value);
    void renderPending();
    void renderPaid(qint64 amountCents, qint64 balanceAfterCents);
    void refreshAffordability();
    void requestPay();
    void setPaying(bool busy);

    ChargingService* service_ = nullptr;
    charging::client::ChargingStatus stopped_;
    qint64 balanceCents_ = -1; // -1: unknown, never blocks the button blindly
    bool paidShown_ = false;

    Card* pendingCard_ = nullptr;
    Card* paidCard_ = nullptr;
    QLabel* amountLabel_ = nullptr;
    QLabel* stationLabel_ = nullptr;
    QLabel* balanceLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    ActionButton* backButton_ = nullptr; // 页内返回（嵌入壳层时隐藏）
    ActionBar* payBar_ = nullptr;        // 待支付态底部操作条
    ActionBar* doneBar_ = nullptr;       // 支付完成态底部操作条
    QVBoxLayout* infoRowsLayout_ = nullptr;
    ActionButton* payButton_ = nullptr;
    ActionButton* rechargeButton_ = nullptr;
    ActionButton* doneButton_ = nullptr;
    QLabel* paidAmountLabel_ = nullptr;
    QLabel* paidBalanceLabel_ = nullptr;

    LoadingOverlay* overlay_ = nullptr;
};

} // namespace charging::client
