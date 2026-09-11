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
namespace services::settings {
class SettingsService;
} // namespace services::settings
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
//
// 数据流：本页自身不发网络请求——站点/桩数据经 StationQueryService 详情通道
// 获取（模拟 ↔ TCP 真实通道对 UI 透明，liveMode 下请求 GET_CHARGERS），预约
// 资格仅同步查询 ReservationService/SettingsService 状态，动作出口全部以信号
// 交宿主。
//
// 被谁用：HomeShell（widgets 充电客户端宿主）注入三个服务并接住 5 条预约
// 信号做路由/提示；QML 孪生页口径对照见 docs/design/qml-station-mapping.md。
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
    // 任务 #17 二次迭代：车辆档案来源（默认车辆接口类型用于桩卡片匹配提示
    // 与排序；车辆数用于预约名额判断）。无车辆时点击预约发
    // reservationVehicleRequired 交宿主引导去设置添加。
    void setSettingsService(charging::client::services::settings::SettingsService* settings);
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
    // 错误页“返回首页”→ 宿主切回找站列表（全局 TopNavBar 的“返回”不经过本页）。
    void backRequested();
    // 预约入口点击（携带桩 ID，宿主/测试可观察）。
    void reservationRequested(qint64 chargerId);
    // 未登录点击预约：宿主提示登录并跳转登录页。
    void reservationLoginRequired();
    // 账号下无车辆被拦截（任务 #17 二次迭代：名额由车辆决定）：
    // 宿主弹提示引导去「设置 - 车辆管理」添加车辆。
    void reservationVehicleRequired();
    // 可预约名额已全部占用被拦截（名额制业务约束）：宿主弹提示。
    void reservationBlocked();
    // 满足预约条件：宿主路由至独立预约确认页面（任务 #17 迭代，携带
    // 站点快照 / 桩 / 虚拟导航距离）。
    void reservationConfirmRequested(const charging::model::Station& station,
                                     const charging::model::Charger& charger,
                                     int distanceMeters);

private:
    // ---- 状态机与渲染（内部） ----

    // 三态切换唯一入口：viewState_（探针口径）与 pageStack_ 当前页同步变更，
    // 其它地方不绕过它直接 setCurrentIndex。
    void setDetailState(DetailState state);
    // 重建前清空旧桩卡：widget 走 deleteLater（重拉在信号槽栈内同步发生）。
    void clearChargerRows();
    // 单桩卡片：编号 + 状态标签 / 类型·功率 / 预约按钮（灰化、匹配提示内置），
    // isChargerCard/chargerId/chargerStatus 属性即测试契约。
    QWidget* createChargerCard(const charging::model::Charger& charger);
    // 桩接口类型与默认车辆匹配（无车辆/无默认车时视为匹配，不做筛选）。
    bool matchesDefaultVehicle(const charging::model::Charger& charger) const;
    // ---- 预约出口与服务回调（内部） ----
    // 三级拦截链（登录 → 车辆 → 名额），顺序与豁免理由见 .cpp 同名函数。
    void handleReserveRequested(const charging::model::Charger& charger);
    // 服务详情通道三段信号：开始=回加载（防旧数据滞留），成功=数据源回写并
    // 重建桩卡，失败=全屏错误态。
    void handleDetailStarted();
    void handleDetailSucceeded(const services::station::StationDetail& detail);
    void handleDetailFailed(const QString& message);

    services::station::StationQueryService* service_ = nullptr;  // not owned
    services::reservation::ReservationService* reservationService_ = nullptr; // not owned
    services::settings::SettingsService* settings_ = nullptr; // not owned
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
    // -1 = 未知距离（formatDistance 显示 “--”）；同时随 confirm 信号透传给
    // 预约确认页，保证两页距离口径同源。
    int lastDistanceMeters_ = -1;
};

} // namespace charging::client::pages::station
