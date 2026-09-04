#pragma once

#include "charging/client/widgets/bottom_tab_bar.h"
#include "charging/common/model/models.h"

#include <QWidget>

class QStackedWidget;

namespace charging::client {
class TopNavBar;
namespace network {
class ClientConnection;
}
namespace services::reservation {
class ReservationService;
} // namespace services::reservation
}

namespace charging::client::pages::station {

class ReservationConfirmPage;
class ReservationModulePage;
class StationDetailPage;
class StationHomePage;

// 首页导航外壳（成员 2，任务 #2/#7）。
//
// 复用全局公共组件 TopNavBar（顶部导航）与 BottomTabBar（底部 Tab），
// 所有用户端页面统一沿用这套导航/样式/交互。底部四个 Tab 固定为：
// 找站（首页，默认激活）/ 订单 / 充值 / 我的；内容区为 QStackedWidget。
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
class HomeShell final : public QWidget
{
    Q_OBJECT

public:
    // 已登录：user 透传给导航组件。
    explicit HomeShell(const charging::model::User& user, QWidget* parent = nullptr);
    // 未登录：首页仍可渲染，右上角显示登录按钮。
    explicit HomeShell(QWidget* parent = nullptr);

    // 注入网络层：站点查询服务保留真实接口通道（服务端 GET_STATIONS
    // 就绪后开启 setLiveMode(true) 即可切换，页面 UI 逻辑不变）。
    void setConnection(charging::client::network::ClientConnection* connection);

    StationHomePage* stationPage() const;
    ReservationModulePage* reservationModule() const;

signals:
    // 已登录时用户点击“退出登录”，请求返回登录页。
    void logoutRequested();
    // 未登录时用户点击顶部登录按钮（或“我的”页的“立即登录”）。
    void loginRequested();

private:
    explicit HomeShell(const charging::model::User* user, QWidget* parent);

    enum class Route
    {
        None,
        StationDetail,
        ReservationConfirm, // 独立预约确认页面（任务 #17 迭代，替代弹窗）
        ReservationModule,  // 我的预约模块（二级 Tab 容器）
    };

    void showTab(const QString& id);
    void openStationDetail(const charging::model::Station& station, int distanceMeters);
    void openReservationConfirm(const charging::model::Station& station,
                                const charging::model::Charger& charger, int distanceMeters);
    void openReservationModule();
    void leaveRoute();
    void showReservationLoginPrompt();
    void showUnfinishedReservationPrompt();
    QWidget* createOrderPage();
    QWidget* createRechargePage();
    QWidget* createProfilePage();

    charging::client::TopNavBar* topBar_ = nullptr;
    QStackedWidget* pageStack_ = nullptr;
    charging::client::BottomTabBar* tabBar_ = nullptr;
    StationHomePage* stationPage_ = nullptr;
    StationDetailPage* detailPage_ = nullptr;
    ReservationConfirmPage* confirmPage_ = nullptr;
    ReservationModulePage* modulePage_ = nullptr;
    charging::client::services::reservation::ReservationService* reservationService_ = nullptr;
    Route route_ = Route::None;
    charging::model::User user_;
    bool hasUser_ = false;
};

} // namespace charging::client::pages::station
