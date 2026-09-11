#include "pages/station/station_map_panel.h"

#include <QtTest>

// StationMapPanel 当前不挂到首页（站点/地图业务属任务 #7），
// 但组件与降级逻辑已就绪，这里保证任务 #7 接入前行为不回退。
//
// boot：QTEST_MAIN 起 QApplication 直构 QWidget 面板（不 show）；面板可用需
// WebEngine + CHARGING_TENCENT_MAP_KEY 双条件，本测试只钉"无 Key ⇒ 必降级"
// 这条与构建无关的确定路径——CI/offscreen 下同样稳定，不碰网络、不依赖凭证。
class StationMapPanelTest final : public QObject
{
    Q_OBJECT

private slots:
    void degradesWithoutMapKey();
    void degradesWithEmptyKeyAfterStationsSet();
};

// 测：完全未注入 Key 时构造即降级；钉需求 #22 的降级路径——无凭证环境
// （CI/offscreen）下面板可实例化且明确自报降级，不会半初始化崩给宿主。
void StationMapPanelTest::degradesWithoutMapKey()
{
    // 测试环境必须不依赖真实地图服务：不注入 Key 时应直接降级（需求 #22）。
    qputenv("CHARGING_TENCENT_MAP_KEY", "");

    charging::client::pages::station::StationMapPanel panel;
    QVERIFY(panel.isDegraded());
}

// 测：仅空白的 Key（"   "）视同未配置，且 setStations 注入数据后仍降级；
// 钉空白凭证等价于无凭证（防绕过）、setStations 在降级态只缓存不重建地图
// ——否则会渲染一张无凭证的空地图糊在列表上方。
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
