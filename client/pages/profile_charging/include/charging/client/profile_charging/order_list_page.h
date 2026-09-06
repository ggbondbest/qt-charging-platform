#pragma once

#include "charging/client/profile_charging/order_service.h"
#include "charging/common/protocol/protocol.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace charging::client {

class ActionButton;
class LoadingOverlay;
class NoticePanel;
class PullToRefreshArea;

// Order history list: filter chips, paged rows, Loading/Empty/Error states.
// Rows are grouped under month headers ("2026年9月" + 单数/金额/电量 汇总);
// the sort and the sums are display-only conveniences over the same
// server-authoritative rows, never new protocol state.
// Pure presentation: every row comes from OrderService, which mirrors the
// server-side list. Opening a row only navigates; it never changes state.
class OrderListPage final : public QWidget
{
    Q_OBJECT

public:
    explicit OrderListPage(OrderService* service, QWidget* parent = nullptr);

    // Re-fetch the first page for the current filter; called on entry.
    void refresh();

    // Enter the list on a specific filter (used by the profile hub badge
    // cells); switches the chip row and re-fetches.
    void showFilter(OrderService::Filter filter);
    // 整合壳层（HomeShell）内使用：本页为底部 Tab 根页，返回按钮无意义，
    // 嵌入时隐藏（信号保留，独立预览仍可用）。
    void setEmbedded(bool embedded);

signals:
    void backRequested();
    void orderOpened(const charging::client::OrderSummary& order);

private slots:
    void onOrdersLoaded(const QVector<charging::client::OrderSummary>& orders, int total,
                        bool hasMore);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    QWidget* buildOrderRow(const charging::client::OrderSummary& summary);
    QWidget* buildMonthHeader(const QString& monthKey, QLabel** summaryOut);
    void rebuildMonthGroups();
    void applyFilter(OrderService::Filter filter);
    void clearOrderRows();
    void showListNotice(const QString& glyph, const QString& title, const QString& description,
                        const QString& actionText);
    void hideListNotice();
    void beginBusy();
    void endBusy();

    OrderService* service_ = nullptr;

    ActionButton* backButton_ = nullptr; // 页内返回（嵌入壳层 Tab 时隐藏）
    QVector<ActionButton*> filterChips_;
    QVector<OrderService::Filter> filterValues_;
    bool applyingFilter_ = false;
    OrderService::Filter currentFilter_ = OrderService::Filter::All;

    PullToRefreshArea* listScroll_ = nullptr;
    QStackedWidget* listStack_ = nullptr; // 列表 / 空态·错误态提示互斥（提示铺满列表区）
    QVBoxLayout* listLayout_ = nullptr;
    ActionButton* loadMoreButton_ = nullptr;
    NoticePanel* listNotice_ = nullptr;

    LoadingOverlay* overlay_ = nullptr;
    int busyCount_ = 0;

    QVector<charging::client::OrderSummary> shownOrders_;
    int currentPage_ = 0; // Last fully loaded page.
    int loadingPage_ = 0; // Page requested by the currently in-flight call.
    bool hasMoreOrders_ = false;
    int animateRowsFrom_ = 0; // 行入场动效起点（首屏=0 全量 stagger；"加载更多"=旧行数）。
};

} // namespace charging::client
