#pragma once

#include "charging/client/profile_charging/wallet_service.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;

namespace charging::client {

class ActionButton;
class LoadingOverlay;

// Recharge page: preset amount chips + custom amount input, confirmation
// dialog and result feedback. The final balance always comes back through
// WalletService signals; the page never mutates it locally.
class RechargePage final : public QWidget
{
    Q_OBJECT

public:
    explicit RechargePage(WalletService* service, QWidget* parent = nullptr);

    // Shows the balance captured when the page was entered (display only).
    void setBalance(qint64 balanceCents);
    void resetForm();
    // 整合壳层（HomeShell）内使用：顶部全局导航已提供返回，隐藏页内返回
    // 按钮，避免双返回（信号保留，独立预览仍可用）。
    void setEmbedded(bool embedded);

signals:
    void backRequested();
    void rechargeSucceeded(qint64 balanceAfterCents);

protected:
    // 每次进入页面刷新"结果未确认充值"提示条（上次会话超时留下的意图可能
    // 已在服务端入账，契约 v1 §3：重试沿用原流水号幂等确认，不会重复入账）。
    void showEvent(QShowEvent* event) override;

private slots:
    void onConfirmClicked();
    void onRechargeCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    void setSubmitting(bool submitting);
    // Returns selected amount in cents, or -1 when the selection is invalid.
    qint64 selectedAmountCents(QString* invalidReason) const;
    void updatePendingRechargeNotice();

    WalletService* service_ = nullptr;

    ActionButton* backButton_ = nullptr;
    QLabel* balanceValueLabel_ = nullptr;
    QWidget* pendingBar_ = nullptr; // 未确认充值恢复条，无 pending 意图时隐藏
    QLabel* pendingNoticeLabel_ = nullptr;
    ActionButton* retryButton_ = nullptr;
    QVector<ActionButton*> amountChips_;
    QVector<qint64> chipAmountsCents_;
    QLineEdit* customAmountEdit_ = nullptr;
    ActionButton* confirmButton_ = nullptr;
    LoadingOverlay* overlay_ = nullptr;
};

} // namespace charging::client
