#pragma once

#include "charging/client/profile_charging/order_service.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;

namespace charging::client {

class NoticePanel;
class StatusTag;

// 「我的」Tab 根页（个人中心）：仿主流充电 App 的“我的”版式——
// 渐变头图（头像/昵称/手机号，整块点击进编辑资料）、压住头图下沿的白色
// 钱包浮卡（余额/充值/充值记录三列）、我的订单与我的预约双格入口，
// 底部设置分组行与退出登录。页面只渲染缓存数据，所有业务数字以服务端
// 为准（refresh() 每次进入重查）。
class ProfilePage final : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilePage(WalletService* walletService, OrderService* orderService,
                         QWidget* parent = nullptr);

    // 进入页面即重查资料与订单角标，避免展示过期余额/数量。
    void refresh();

    // 壳层注入真实登录用户：先同步渲染身份（余额等字段随后由服务端回填）。
    void setIdentity(const charging::model::User& user);

signals:
    void profileEditRequested();
    void walletRequested();           // 钱包页路由入口（余额/充值记录）
    void rechargeRequested();
    void allOrdersRequested();
    void reservationRecordsRequested(); // 我的预约（成员 2 模块，壳路由）
    void settingsRequested();           // 设置（成员 2 设置页，壳路由）
    void favoritesRequested();          // 收藏（成员 2 迭代 3 收藏夹页，壳路由）
    void logoutRequested();

private slots:
    void onProfileLoaded(const charging::model::User& user);
    void onStatusCounts(int chargingCount, int waitingPaymentCount, int completedCount);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    void renderIdentity(const charging::model::User& user); // setIdentity/异步加载共用

    WalletService* walletService_ = nullptr;
    OrderService* orderService_ = nullptr;

    QLabel* avatarLabel_ = nullptr;
    QLabel* identityNameLabel_ = nullptr;
    QLabel* identityPhoneLabel_ = nullptr;
    QLabel* balanceValueLabel_ = nullptr; // 测试锚点 balanceLabel
    StatusTag* waitingBadge_ = nullptr;   // 订单格“待支付 n”角标

    NoticePanel* profileNotice_ = nullptr;

    charging::model::User user_;
    bool hasUser_ = false;
};

} // namespace charging::client
