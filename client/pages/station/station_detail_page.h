#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;
class QStackedWidget;
class QVBoxLayout;

namespace charging::client {
class NoticePanel;
class StatusTag;
namespace services::station {
class StationQueryService;
struct StationDetail;
} // namespace services::station
namespace services::reservation {
class ReservationService;
} // namespace services::reservation
} // namespace charging::client

namespace charging::client::pages::station {

// 站点详情页（成员 2，任务 #12/#17/#17迭代）。
//
// 页面结构：站点基础信息卡（名称/地址/状态）→ 电价与距离 → 离线横幅
// （站点 Inactive 时醒目提示）→ 充电桩卡片列表（编号/类型/功率/工作状态）。
//
// 多边界状态（规格要求全覆盖）：
// - 加载中：进入页面后向 StationQueryService 请求桩列表；
// - 空数据：站点正常但无充电桩 → 列表区展示“暂无充电桩”；
// - 站点离线：信息区状态标签 + 醒目离线横幅，桩位全部按数据源渲染；
// - 故障标记：故障桩卡片红色视觉标记（属性选择器，页面局部样式）；
// - 接口/网络异常（含无站点 ID、ID 非法）：全屏错误态 + “返回首页”按钮。
//
// 模拟 ↔ 真实 Service 无缝：数据只经 StationQueryService 详情通道获取，
// UI 不感知通道来源。导航复用全局 TopNavBar（返回按钮由宿主壳控制显隐），
// 本页不重复实现导航代码。任务 #17 迭代：桩卡片“预约”按钮不再打开弹窗，
// 满足条件（已登录 + 无未结束预约）时发 reservationConfirmRequested 由宿主
// 路由至独立预约确认页面；存在未结束预约时发 reservationBlocked 由宿主
// 提示拦截；未登录点击发 reservationLoginRequired 交宿主拦截跳登录。
// 桩列表置于 QScrollArea，鼠标滚轮上下滚动。
class StationDetailPage final : public QWidget
{
    Q_OBJECT

public:
    enum class DetailState
    {
        Loading, // 拉取桩列表中
        Error,   // 站点 ID 非法 / 接口或网络异常
        Ready,   // 信息 + 桩列表（列表区内部再分 空/正常）
    };

    explicit StationDetailPage(QWidget* parent = nullptr);

    // 非拥有：与列表页共用同一服务实例（HomeShell 注入）。
    void setService(charging::client::services::station::StationQueryService* service);

    // 任务 #17 迭代：预约服务（入口拦截判断 + 确认页/模块共用同一实例）
    // 与登录态透传（由 HomeShell 注入，未登录时点击预约发
    // reservationLoginRequired 拦截）。
    void setReservationService(charging::client::services::reservation::ReservationService* service);
    void setLoggedIn(bool loggedIn);

    // 路由入口：携带站点快照与距离（ID 非法时服务将回友好错误）。
    void openStation(const charging::model::Station& station, int distanceMeters);

    // 预约成功后由宿主回灌：刷新当前充电桩状态（模拟通道本地置为“已预约”
    // 后重拉；真实通道以服务端数据为准），任务 #17 迭代自弹窗逻辑迁入。
    void noteChargerReserved(qint64 chargerId);

    // 测试探针（isVisibleTo 语义：不依赖整页是否已被宿主显示）。
    DetailState viewState() const;
    int chargerCardCount() const;
    bool offlineBannerVisible() const;
    bool chargerEmptyVisible() const;

signals:
    void backRequested();
    // 预约入口点击（携带桩 ID，宿主/测试可观察）。
    void reservationRequested(qint64 chargerId);
    // 未登录点击预约：宿主提示登录并跳转登录页。
    void reservationLoginRequired();
    // 存在未结束预约被拦截（任务 #17 迭代业务约束）：宿主弹提示。
    void reservationBlocked();
    // 满足预约条件：宿主路由至独立预约确认页面（任务 #17 迭代，携带
    // 站点快照 / 桩 / 虚拟导航距离）。
    void reservationConfirmRequested(const charging::model::Station& station,
                                     const charging::model::Charger& charger,
                                     int distanceMeters);

private:
    void setDetailState(DetailState state);
    void clearChargerRows();
    QWidget* createChargerCard(const charging::model::Charger& charger);
    void handleReserveRequested(const charging::model::Charger& charger);
    void handleDetailStarted();
    void handleDetailSucceeded(const services::station::StationDetail& detail);
    void handleDetailFailed(const QString& message);

    services::station::StationQueryService* service_ = nullptr;  // not owned
    services::reservation::ReservationService* reservationService_ = nullptr; // not owned
    bool loggedIn_ = true;
    DetailState viewState_ = DetailState::Loading;

    QStackedWidget* pageStack_ = nullptr; // Loading / Error / Ready
    QWidget* loadingPage_ = nullptr;
    NoticePanel* errorNotice_ = nullptr;

    // Ready 页内容。
    QLabel* nameLabel_ = nullptr;
    QLabel* addressLabel_ = nullptr;
    QLabel* priceLabel_ = nullptr;
    QLabel* distanceLabel_ = nullptr;
    charging::client::StatusTag* statusTag_ = nullptr;
    QLabel* offlineBanner_ = nullptr;
    QLabel* chargerSummaryLabel_ = nullptr;
    QStackedWidget* chargerStack_ = nullptr; // 加载中(隐藏占位)/空/列表
    QWidget* chargerEmptyNotice_ = nullptr;
    QWidget* chargerListPage_ = nullptr;
    QVBoxLayout* chargerListLayout_ = nullptr;

    charging::model::Station station_;
    int lastDistanceMeters_ = -1;
};

} // namespace charging::client::pages::station
