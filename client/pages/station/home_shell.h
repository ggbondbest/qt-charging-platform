#pragma once

#include "charging/client/profile_charging/order_service.h"
#include "charging/client/widgets/bottom_tab_bar.h"
#include "charging/common/model/models.h"

#include <QString>
#include <QVector>
#include <QWidget>

#include <optional>

class QStackedWidget;

namespace charging::client {
class TopNavBar;
class MockRequestTransport;
class WalletService;
class OrderService;
class ChargingService;
class ProfilePage;
class WalletPage;
class OrderListPage;
class OrderDetailPage;
class ChargingPage;
class ChargingHomePage;
class SettlementPage;
class RechargePage;
class ProfileEditPage;
namespace network {
class ClientConnection;
}
namespace services::map {
class MapGeoService;
} // namespace services::map
namespace services::favorites {
class FavoritesService;
class NotificationService;
} // namespace services::favorites
namespace services::reservation {
class ReservationService;
struct ReservationRecord;
} // namespace services::reservation
namespace services::settings {
class SettingsService;
} // namespace services::settings
}

namespace charging::client::pages::station {

class ReservationConfirmPage;
class ReservationModulePage;
class StationDetailPage;
class StationHomePage;
class SettingsPage;
class NavigationPage;
class FavoritesPage;
class NotificationPage;

// 首页导航外壳（成员 2，任务 #2/#7）。
//
// 复用全局公共组件 TopNavBar（顶部导航）与 BottomTabBar（底部 Tab），
// 所有用户端页面统一沿用这套导航/样式/交互。底部四个 Tab 固定为：
// 找站（首页，默认激活）/ 订单 / 充电 / 我的；内容区为 QStackedWidget。
//
// 登录态透传：User 经 setUser 进入 TopNavBar 与“我的”页；未登录构造的壳
// 右上角显示登录按钮，点击发 loginRequested 由宿主（MainWindow）跳登录页。
//
// 任务 #7：顶部搜索框驱动找站页站点检索；站点卡片点击 → 详情路由。
// 任务 #12：详情页与列表共用同一查询服务实例；进入详情时顶部导航显示
// “返回”按钮（复用全局 TopNavBar，不新增页面级导航），返回/切 Tab 收起。
// 任务 #17 迭代：预约服务（ReservationService）统一注入详情页、独立预约
// 确认页面（索引 5 路由页，替代原弹窗）与“我的预约”模块页（索引 6 路由
// 页，二级 Tab：预约订单 / 已完成的预约）；存在未结束预约时入口拦截提示；
// 未登录点击提示登录。
//
// 全端整合（成员 3）：三个占位 Tab 换成成员 3 真实页面——订单 →
// OrderListPage、充电 Tab → ChargingHomePage（按充电生命周期状态展示：
// 充电中/待支付/已有预约/无任务），钱包页降为路由页（入口在「我的」
// 钱包卡）；我的 → ProfilePage 中心页（含编辑资料/详情/结算/充值/
// 钱包/充电过程路由页；充电中订单点击进入 ChargingPage，停止 → 结算
// 闭环，充值成功自动回跳结算并解锁支付）；钱包/订单/充电服务在真实登录
// （带 connection）时走契约 v1 的 NetworkRequestTransport，仅无连接预览
// 用共享 mock 通道。“我的预约”入口与
// 退出登录按钮保留原测试锚点 objectName（openReservationsButton/logoutButton/
// nicknameLabel/balanceLabel）。路由返回改为返回栈（backTargets_），支持
// “详情→结算”等多级返回；切任意 Tab 清栈。
// 任务 #17 二次迭代追加：设置页（成员 3 ProfilePage“设置”行进入，返回栈
// 固定回“我的”Tab）与导航页（预约成功“去充电”弹窗进入，返回链
// 导航 → 预约模块【预约订单】→“我的”Tab）。
// 迭代 3 追加：收藏服务 + 消息通知服务为壳内单实例（预约信号桥接实时
// 生成通知）；消息通知页（顶部铃铛进入，登录后 15 / 未登录 9）与收藏夹
// 页（“我的 → 收藏”进入，登录后 16 / 未登录 10）追加在栈尾；两者未登录
// 一律拦截提示登录；顶部筛选按钮直达找站页高级筛选弹窗。
class HomeShell final : public QWidget
{
    Q_OBJECT

public:
    // 已登录：user 透传给导航组件。
    explicit HomeShell(const charging::model::User& user, QWidget* parent = nullptr);
    HomeShell(const charging::model::User& user,
              charging::client::network::ClientConnection* connection, QWidget* parent);
    // 未登录：首页仍可渲染，右上角显示登录按钮。
    explicit HomeShell(QWidget* parent = nullptr);

    // 注入并开启站点/预约真实通道。生产入口必须使用带 connection 的构造函数，
    // 以便个人/订单/钱包服务也在构造时选择真实适配器。
    void setConnection(charging::client::network::ClientConnection* connection);

    StationHomePage* stationPage() const;
    ReservationModulePage* reservationModule() const;
    // 设置服务/设置页（测试注入车辆档案驱动名额与默认车约束）。
    charging::client::services::settings::SettingsService* settingsService() const;
    // 迭代 3 探针：收藏/通知服务与两个新路由页（测试直接驱动断言）。
    charging::client::services::favorites::FavoritesService* favoritesService() const;
    charging::client::services::favorites::NotificationService* notificationService() const;
    FavoritesPage* favoritesPage() const;
    NotificationPage* notificationPage() const;

    // 路由入口（UI 内经个人中心卡片/引导弹窗触发；宿主与测试可直接调用）。
    void openReservationModule();
    void openSettings();
    // 迭代 3：消息通知页（顶部铃铛）与收藏夹页（“我的 → 收藏”）；
    // 未登录一律拦截提示登录。
    void openNotifications();
    void openFavorites();

signals:
    // 已登录时用户点击“退出登录”，请求返回登录页。
    void logoutRequested();
    // 未登录时用户点击顶部登录按钮（或“我的”页的“立即登录”）。
    void loginRequested();

private:
    explicit HomeShell(const charging::model::User* user, QWidget* parent,
                       charging::client::network::ClientConnection* connection = nullptr);

    // 路由返回栈条目：回 Tab 页记 tabId（切 Tab 顺带清路由态），
    // 回路由页记 page（保持返回按钮可见，支持多级返回）。
    struct BackTarget
    {
        QWidget* page = nullptr;
        QString tabId;
    };

    void showTab(const QString& id);
    void openStationDetail(const charging::model::Station& station, int distanceMeters);
    void openReservationConfirm(const charging::model::Station& station,
                                const charging::model::Charger& charger, int distanceMeters);
    // 全端整合：成员 3 路由页统一入口（记录当前位置 → 切页 → 显示返回）。
    void pushRoute(QWidget* page);
    void openOrderDetail(const charging::client::OrderSummary& summary);
    void openSettlement();
    void openRecharge();
    void openWallet();
    void openProfileEdit();
    void openNavigation(const services::reservation::ReservationRecord& record);
    void leaveRoute();
    // 顶栏态与路由栈/当前 Tab 同步：返回按钮跟随路由栈，搜索框只留在「找站」。
    void syncTopBar();
    void showReservationLoginPrompt();
    // 迭代 3：通用登录拦截提示（通知/收藏入口复用；文案随功能定制，
    // 对象名与预约拦截一致，宿主“去登录”同走 loginRequested）。
    void showFeatureLoginPrompt(const QString& text, const QString& informativeText);
    void showUnfinishedReservationPrompt();
    void showNoVehiclePrompt();
    void showGoChargePrompt(const services::reservation::ReservationRecord& record);
    QWidget* createOrderPage();
    QWidget* createChargingPage();
    QWidget* createProfilePage();

    charging::client::TopNavBar* topBar_ = nullptr;
    QStackedWidget* pageStack_ = nullptr;
    charging::client::BottomTabBar* tabBar_ = nullptr;
    StationHomePage* stationPage_ = nullptr;
    StationDetailPage* detailPage_ = nullptr;
    ReservationConfirmPage* confirmPage_ = nullptr;
    ReservationModulePage* modulePage_ = nullptr;
    SettingsPage* settingsPage_ = nullptr;    // 路由页（登录后 12；登录前 7）
    NavigationPage* navigationPage_ = nullptr; // 路由页（登录后 13；登录前 8）
    FavoritesPage* favoritesPage_ = nullptr;   // 迭代 3 路由页（登录后 16；登录前 10）
    NotificationPage* notificationPage_ = nullptr; // 迭代 3 路由页（登录后 15；登录前 9）
    charging::client::services::reservation::ReservationService* reservationService_ = nullptr;
    charging::client::services::settings::SettingsService* settingsService_ = nullptr;
    charging::client::services::map::MapGeoService* mapGeoService_ = nullptr;
    charging::client::services::favorites::FavoritesService* favoritesService_ = nullptr;
    charging::client::services::favorites::NotificationService* notificationService_ = nullptr;
    QVector<BackTarget> backTargets_;

    // ---- 成员 3 整合：传输通道（真实登录 live / 预览 mock）+ 服务 + 页面 ----
    charging::client::MockRequestTransport* mockTransport_ = nullptr;
    charging::client::WalletService* walletService_ = nullptr;
    charging::client::OrderService* orderService_ = nullptr;
    charging::client::ChargingService* chargingService_ = nullptr;
    charging::client::ProfilePage* profilePage_ = nullptr;    // “我的”Tab（登录后）
    charging::client::WalletPage* walletPage_ = nullptr;      // 路由页（“我的”钱包卡进入）
    charging::client::ChargingHomePage* chargingHomePage_ = nullptr; // “充电”Tab（状态首页）
    charging::client::OrderListPage* orderListPage_ = nullptr; // “订单”Tab
    charging::client::OrderDetailPage* orderDetailPage_ = nullptr; // 路由页 7
    charging::client::SettlementPage* settlementPage_ = nullptr;   // 路由页 8
    charging::client::RechargePage* rechargePage_ = nullptr;       // 路由页 9
    charging::client::ProfileEditPage* profileEditPage_ = nullptr; // 路由页 10
    charging::client::ChargingPage* chargingPage_ = nullptr;       // 路由页 11（充电中订单）
    charging::client::OrderSummary currentSummary_;                // 详情/结算路由上下文
    qint64 lastKnownBalanceCents_ = 0;
    std::optional<charging::client::OrderService::Filter> pendingOrderFilter_;
    charging::model::User user_;
    bool hasUser_ = false;
};

} // namespace charging::client::pages::station
