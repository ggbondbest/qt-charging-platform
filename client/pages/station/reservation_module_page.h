#pragma once

#include "services/reservation/reservation_service.h"

#include <QWidget>

class QPushButton;
class QStackedWidget;

namespace charging::client::pages::station {

class ReservationCompletedPage;
class ReservationOrderPage;

// 预约模块容器页（成员 2，任务 #17 迭代）：“我的预约”拆分为两个独立
// 页面，通过模块内**二级 Tab**切换（仅在本模块内生效，不改动全局底部
// Tab 外壳——导航复用任务 #2 公共组件）：
// - 【预约订单】：进行中的预约（三栏布局 + 每秒倒计时）；
// - 【已完成的预约】：历史预约归档列表（点击卡片查看详情）。
//
// 模块统一持有 ReservationService 列表通道：一次 fetchList 结果按状态
// 分发（预约中 → 订单页；已完成/已取消/已过期 → 已完成页），加载/错误
// 态同步驱动两个子页；取消成功后按规格自动切换到【已完成的预约】Tab。
// 倒计时归零流转（reservationExpired）触发模块重新拉取。
class ReservationModulePage final : public QWidget
{
    Q_OBJECT

public:
    explicit ReservationModulePage(QWidget* parent = nullptr);

    // 非拥有：与详情页/确认页同一服务实例（HomeShell 注入）。
    void setService(charging::client::services::reservation::ReservationService* service);

    // 进入模块：拉取最新列表（未注入服务时两页展示友好错误）。
    void refresh();

    // 二级 Tab 控制（宿主：预约成功 → 订单页；“我的”入口默认订单页）。
    void showOrderTab();
    void showCompletedTab();
    QString currentSubTab() const; // "order" / "completed"

    // 测试探针。
    ReservationOrderPage* orderPage() const;
    ReservationCompletedPage* completedPage() const;
    // 非拥有访问器（测试/宿主演示分支开关用）。
    charging::client::services::reservation::ReservationService* service() const;

signals:
    // 订单页空态“去找桩”：宿主路由回“找站”Tab。
    void findStationRequested();

private:
    void handleListSucceeded(const services::reservation::ReservationList& records);
    void switchTab(const QString& id);

    charging::client::services::reservation::ReservationService* service_ = nullptr;

    QPushButton* orderTabButton_ = nullptr;
    QPushButton* completedTabButton_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    ReservationOrderPage* orderPage_ = nullptr;
    ReservationCompletedPage* completedPage_ = nullptr;
};

} // namespace charging::client::pages::station
