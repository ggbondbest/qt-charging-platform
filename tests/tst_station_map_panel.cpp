#include "pages/station/station_map_panel.h"

#include <QtTest>

// StationMapPanel 当前不挂到首页（站点/地图业务属任务 #7），
// 但组件与降级逻辑已就绪，这里保证任务 #7 接入前行为不回退。
class StationMapPanelTest final : public QObject
{
    Q_OBJECT

private slots:
    void degradesWithoutMapKey();
    void degradesWithEmptyKeyAfterStationsSet();
};

void StationMapPanelTest::degradesWithoutMapKey()
{
    // 测试环境必须不依赖真实地图服务：不注入 Key 时应直接降级（需求 #22）。
    qputenv("CHARGING_TENCENT_MAP_KEY", "");

    charging::client::pages::station::StationMapPanel panel;
    QVERIFY(panel.isDegraded());
}

void StationMapPanelTest::degradesWithEmptyKeyAfterStationsSet()
{
    qputenv("CHARGING_TENCENT_MAP_KEY", "   ");

    charging::client::pages::station::StationMapPanel panel;
    QVector<charging::client::pages::station::MapStationPoint> points;
    points.append({22.5412, 113.9430, QStringLiteral("科技园充电驿站")});
    panel.setStations(points);
    // 仅空白的 Key 视为未配置，仍应降级而不是渲染空地图。
    QVERIFY(panel.isDegraded());
}

QTEST_MAIN(StationMapPanelTest)

#include "tst_station_map_panel.moc"
