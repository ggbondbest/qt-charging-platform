#pragma once

#include "charging/client/profile_charging/order_service.h"

#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace charging::client {

class ActionBar;
class ActionButton;
class Card;
class StatusTag;

// Read-only order detail: everything shown here comes from the order the
// server listed. The pay button only asks the app shell to navigate to the
// settlement flow; it never mutates order state.
class OrderDetailPage final : public QWidget
{
    Q_OBJECT

public:
    explicit OrderDetailPage(QWidget* parent = nullptr);

    void showOrder(const charging::client::OrderSummary& summary);
    // 整合壳层（HomeShell）内使用：隐藏页内返回按钮（全局顶部导航负责返回）。
    void setEmbedded(bool embedded);
    // The order currently on screen; lets the shell forward it to settlement.
    charging::client::OrderSummary currentOrder() const { return summary_; }

signals:
    void backRequested();
    void payRequested();

private:
    void buildUi();
    void addInfoRow(QVBoxLayout* layout, const QString& label, const QString& value);

    Card* summaryCard_ = nullptr;
    QLabel* stationLabel_ = nullptr;
    QLabel* metaLabel_ = nullptr;
    StatusTag* statusTag_ = nullptr;
    QLabel* amountLabel_ = nullptr;
    QLabel* energyLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QVBoxLayout* detailRowsLayout_ = nullptr;
    ActionButton* backButton_ = nullptr; // 页内返回（嵌入壳层时隐藏）
    ActionBar* payBar_ = nullptr;        // 待支付态底部操作条
    ActionButton* payButton_ = nullptr;

    charging::client::OrderSummary summary_;
};

} // namespace charging::client
