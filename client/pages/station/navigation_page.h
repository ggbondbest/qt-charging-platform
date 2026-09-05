#pragma once

#include "services/reservation/reservation_service.h"

#include <QPair>
#include <QWidget>

class QLabel;

namespace charging::client::services::map {
class MapGeoService;
struct RouteResult;
} // namespace charging::client::services::map

namespace charging::client::pages::station {

class StationMapPanel;

// 导航页（成员 2，任务 #17 二次迭代 + 地图接入迭代 + 迭代 3 瘦身）：
// 预约成功后“是否现在前往充电？”选择【去充电】进入的路由页（登录后
// HomeShell 索引 13 / 登录前 8，非底部 Tab；“‹ 返回”由全局 TopNavBar
// 承担，返回预约订单页）。
//
// 数据来源（双口径）：
// - 默认/兜底：模拟口径（虚拟导航距离 + recommendSlot 分钟数），caption
//   标注“导航路线为模拟数据”；
// - 腾讯路线规划 API（MapGeoService，key 经环境变量注入且预约记录带
//   站点坐标时）：openRoute 先渲染模拟口径（永不空页），真实路线到达后
//   原地替换距离/时长并把 caption 切到“真实导航路线”；接口异常 Toast
//   提示并保持模拟口径。未注入服务 = 纯模拟（原行为）。
// - 路线步骤文字列表已按产品口径整块移除（迭代 3：模拟路段文案与真实
//   地理不符），本页只做地图路线可视化展示。
class NavigationPage final : public QWidget
{
    Q_OBJECT

public:
    explicit NavigationPage(QWidget* parent = nullptr);

    // 腾讯地图服务（可空）：见类注释。HomeShell 统一注入。
    void setMapService(charging::client::services::map::MapGeoService* mapService);

    // 路由入口：依据预约记录渲染路线（桩站点/编号/时段/距离）。
    void openRoute(const charging::client::services::reservation::ReservationRecord& record);

    // 测试探针。
    QString distanceText() const;
    QString etaText() const;
    bool usingRealRoute() const; // 当前展示的是真实路线数据

private:
    // 顶部地图通道（成员 2 地图渲染迭代）：真实折线优先、模拟折线兜底，
    // 面板不可用（无 WebEngine / 降级）时静默跳过。
    void updateRouteMap();
    void handleRouteResult(quint64 requestId,
                           const charging::client::services::map::RouteResult& route);
    void handleRouteFailure(quint64 requestId, const QString& message);
    // 逆地理（可选接口）：成功给“前往”行补真实地址，失败静默保持站名。
    void handleGeocodeResult(quint64 requestId, const QString& address);

    charging::client::services::reservation::ReservationRecord record_;
    charging::client::services::map::MapGeoService* mapService_ = nullptr;
    quint64 routeGeneration_ = 0; // 过期路线响应过滤
    quint64 geocodeGeneration_ = 0; // 过期逆地理响应过滤
    QVector<QPair<double, double>> realPolyline_; // 真实路线折线（lat,lng）
    bool usingRealRoute_ = false;
    QString defaultCaptionText_;

    StationMapPanel* routeMapPanel_ = nullptr; // 顶部路线地图（可用时）
    QLabel* mapPlaceholderLabel_ = nullptr; // 地图未就绪占位（mapReady 后隐藏）

    QLabel* targetLabel_ = nullptr;
    QLabel* distanceLabel_ = nullptr;
    QLabel* etaLabel_ = nullptr;
    QLabel* captionLabel_ = nullptr;
};

} // namespace charging::client::pages::station
