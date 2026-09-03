#pragma once

#include <QVector>
#include <QWidget>

namespace charging::client {
class NoticePanel;
}

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

    // 地图是否处于降级状态（未渲染真实地图）。
    bool isDegraded() const;

    // 更新电站标记；有地图时重渲染，降级时仅缓存。
    void setStations(const QVector<MapStationPoint>& stations);

signals:
    // 降级提示里的“重试”被点击（由外层决定是否再次尝试加载）。
    void retryRequested();

private:
    static QString mapKey(); // 仅从环境读取；不硬编码进仓库

    QString buildMapHtml() const;
    void showDegraded(const QString& title, const QString& description);
    void tryBuildMapView();

    NoticePanel* degradedNotice_ = nullptr;
    QWidget* mapView_ = nullptr; // 可用时为 QWebEngineView，否则为空
    QVector<MapStationPoint> stations_;
    bool degraded_ = true;
};

} // namespace charging::client::pages::station
