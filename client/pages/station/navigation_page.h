#pragma once

#include "services/reservation/reservation_service.h"

#include <QPair>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace charging::client::pages::station {

// 导航页（成员 2，任务 #17 二次迭代）：预约成功后“是否现在前往充电？”
// 选择【去充电】进入的路由页（HomeShell 索引 8，非底部 Tab；“‹ 返回”
// 由全局 TopNavBar 承担，返回预约订单页）。
//
// 本轮为**模拟路线**：距离/行驶时长沿用预约记录的虚拟导航距离（与推荐
// 时段估算同一来源），路线为模板化分段文字列表。页面顶部 caption 明确
// “导航路线为模拟数据”。腾讯地图 WebService（路线规划/距离矩阵）与
// WebEngine 就绪后的真实接入点已在 cpp 注释块说明——届时仅需替换
// openRoute 内的数据来源，页面结构不变。
class NavigationPage final : public QWidget
{
    Q_OBJECT

public:
    explicit NavigationPage(QWidget* parent = nullptr);

    // 路由入口：依据预约记录渲染模拟路线（桩站点/编号/时段/距离）。
    void openRoute(const charging::client::services::reservation::ReservationRecord& record);

    // 测试探针。
    QString distanceText() const;
    QString etaText() const;
    int routeStepCount() const;

private:
    QPair<QStringList, int> buildMockSteps(
        const charging::client::services::reservation::ReservationRecord& record) const;

    charging::client::services::reservation::ReservationRecord record_;
    QLabel* targetLabel_ = nullptr;
    QLabel* distanceLabel_ = nullptr;
    QLabel* etaLabel_ = nullptr;
    QWidget* stepsHost_ = nullptr;
    QVBoxLayout* stepsLayout_ = nullptr;
};

} // namespace charging::client::pages::station
