#pragma once

#include "charging/client/profile_charging/order_service.h"
#include "charging/common/protocol/protocol.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace charging::client {

class ActionButton;
class LoadingOverlay;
class NoticePanel;

// Order history list: filter chips, paged rows, Loading/Empty/Error states.
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
    void applyFilter(OrderService::Filter filter);
    void clearOrderRows();
    void showListNotice(const QString& glyph, const QString& title, const QString& description,
                        const QString& actionText);
    void hideListNotice();
    void beginBusy();
    void endBusy();

    OrderService* service_ = nullptr;

    QVector<ActionButton*> filterChips_;
    QVector<OrderService::Filter> filterValues_;
    bool applyingFilter_ = false;
    OrderService::Filter currentFilter_ = OrderService::Filter::All;

    QScrollArea* listScroll_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    ActionButton* loadMoreButton_ = nullptr;
    NoticePanel* listNotice_ = nullptr;

    LoadingOverlay* overlay_ = nullptr;
    int busyCount_ = 0;

    QVector<charging::client::OrderSummary> shownOrders_;
    int currentPage_ = 0; // Last fully loaded page.
    int loadingPage_ = 0; // Page requested by the currently in-flight call.
    bool hasMoreOrders_ = false;
};

} // namespace charging::client
