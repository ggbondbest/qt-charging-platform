#pragma once

#include "charging/client/profile_charging/order_service.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/models.h"

#include <QVector>
#include <QWidget>

class QLabel;

namespace charging::client {

class ClickableCard;
class NoticePanel;

// Profile hub (个人中心): the tab root that links into the other member-3
// surfaces, mirroring the mainstream "我的" layout — identity header that
// opens ProfileEditPage, the wallet group (with the balance rendered
// directly in a large circle) above the order group (entry cells carry
// per-status count badges from OrderService::fetchStatusCounts), and a
// disabled "更多" group for modules owned by other members. The hub renders
// cached data only; every business number stays server-authoritative.
class ProfilePage final : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilePage(WalletService* walletService, OrderService* orderService,
                         QWidget* parent = nullptr);

    // Re-fetch profile + order badges on entry so the hub never shows stale
    // balance or counts.
    void refresh();

signals:
    void profileEditRequested();
    void walletRequested();          // 充值记录 (wallet page shows the section)
    void rechargeRequested();
    void allOrdersRequested();
    void chargingOrdersRequested();
    void waitingPaymentOrdersRequested();
    void completedOrdersRequested();

private slots:
    void onProfileLoaded(const charging::model::User& user);
    void onStatusCounts(int chargingCount, int waitingPaymentCount, int completedCount);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    ClickableCard* buildGridCell(const QString& glyph, const QString& caption,
                                 const QString& badgeTone, QLabel** badgeOut);
    static QString badgeText(int count); // "99+" cap; empty for 0.
    void applyBadge(QLabel* badge, const QString& badgeTone, int count);

    WalletService* walletService_ = nullptr;
    OrderService* orderService_ = nullptr;

    QLabel* avatarLabel_ = nullptr;
    QLabel* identityNameLabel_ = nullptr;
    QLabel* identityPhoneLabel_ = nullptr;

    QLabel* balanceCaptionLabel_ = nullptr; // "当前余额（元）" in the banner
    QLabel* balanceValueLabel_ = nullptr;   // "¥ <amount>" in the banner

    QLabel* chargingBadge_ = nullptr;
    QLabel* waitingPaymentBadge_ = nullptr;
    QLabel* completedBadge_ = nullptr;

    NoticePanel* profileNotice_ = nullptr;

    charging::model::User user_;
    bool hasUser_ = false;
};

} // namespace charging::client
