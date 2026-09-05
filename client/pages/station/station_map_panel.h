#pragma once

#include <QVector>
#include <QWidget>

class QLabel;
class QSplitter;

namespace charging::client::pages::station {

// 地图上的电站标记点（经纬度使用十进制度）。
struct MapStationPoint
{
    double latitude = 0.0;
    double longitude = 0.0;
    QString name;
};

// 腾讯地图面板（成员 2，需求 #22：地图入口及无配置降级）。
//
// 可用条件：构建时找到 Qt WebEngine，且运行环境注入了腾讯地图 Key
// （环境变量 CHARGING_TENCENT_MAP_KEY，Key 不得进入仓库）。任一条件不满足时
// 显示可理解的降级提示，主流程（电站列表、预约入口）不受影响。
class StationMapPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit StationMapPanel(QWidget* parent = nullptr);

    // 真实地图的推荐初始展示高度（px）：找站页与导航页的分栏初始尺寸统一
    // 取此值（降级横幅口径另行按行高收小），保证两页地图观感一致。
    static constexpr int kPreferredInitialHeight = 420;

    // 地图是否处于降级状态（未渲染真实地图）。
    bool isDegraded() const;

    // 更新电站标记；有地图时重渲染，降级时仅缓存。
    void setStations(const QVector<MapStationPoint>& stations);

    // 路线折线（导航页）：按顺序连线绘制并自动缩放包住全线路；
    // 传空 = 恢复站点视野（首页口径）。有地图时重渲染，降级时仅缓存。
    void setRoutePoints(const QVector<MapStationPoint>& points);

    // 分栏助手：调用时本面板必须已是 splitter 的第 0 个子件。先按当前
    // 状态给初始尺寸（降级 56 / 真地图 kPreferredInitialHeight，列表半区
    // listPaneInitial），并在地图异步渲染成功时把地图半区升档到推荐高度
    // ——用户手动拖过分栏则不再覆盖（尊重手动选择）。找站页/导航页共用。
    void attachToSplitter(QSplitter* splitter, int listPaneInitial = 300);

signals:
    // 降级提示里的“重试”被点击（由外层决定是否再次尝试加载）。
    void retryRequested();

    // 地图首次渲染成功（异步回调，只发一次）。构造期无法预判是否降级，
    // 外层分栏据此把初始高度从降级值升档到 kPreferredInitialHeight。
    void mapReady();

private:
    static QString mapKey(); // 仅从环境读取；不硬编码进仓库

    QString buildMapHtml() const;
    void showDegraded(const QString& title, const QString& description);
    void tryBuildMapView();

    QWidget* degradedBanner_ = nullptr; // 一行式降级横幅（不再占用整块地图高度）
    QLabel* degradedBannerLabel_ = nullptr;
    QWidget* mapView_ = nullptr; // 可用时为 QWebEngineView，否则为空
    QVector<MapStationPoint> stations_;
    QVector<MapStationPoint> routePoints_; // 导航路线折线（空=首页口径）
    bool degraded_ = true;
    bool mapReadyEmitted_ = false; // mapReady 只在首次渲染成功时发一次
};

} // namespace charging::client::pages::station
