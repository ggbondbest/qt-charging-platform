#include "pages/station/station_map_panel.h"

#include <QtTest>
#include <QFile>
#include <QLabel>
#include <QPointer>
#include <QTemporaryDir>
#ifdef CHARGING_TEST_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#endif

// StationMapPanel 当前不挂到首页（站点/地图业务属任务 #7），
// 但组件与降级逻辑已就绪，这里保证任务 #7 接入前行为不回退。
//
// boot：QTEST_MAIN 起 QApplication 直构 QWidget 面板（不 show）；面板可用需
// WebEngine + 地图配置双条件；有Key的HTML测试拦截所有外部网络请求
// 这条与构建无关的确定路径——CI/offscreen 下同样稳定，不碰网络、不依赖凭证。
class StationMapPanelTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir isolatedConfig_;

private slots:
    void init()
    {
        QFile file(isolatedConfig_.filePath("empty.json"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(R"({"version":1,"tencentMap":{}})");
        file.close();
        qputenv("CHARGING_SERVICES_CONFIG", file.fileName().toUtf8());
        for (const char* name : {"TENCENT_MAP_API_KEY", "CHARGING_TENCENT_MAP_KEY", "TENCENT_MAP_JS_KEY",
                                 "TENCENT_MAP_SECRET_KEY", "CHARGING_TENCENT_MAP_SECRET"}) qputenv(name, "");
    }
#ifdef CHARGING_TEST_WEBENGINE
    void sharedFileSuppliesWidgetsJavaScriptKeyWithoutNetwork()
    {
        class BlockRequests final : public QWebEngineUrlRequestInterceptor {
        public:
            void interceptRequest(QWebEngineUrlRequestInfo& info) override {
                const QString scheme = info.requestUrl().scheme();
                if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
                    info.block(true);
            }
        } blocker;
        auto* profile = QWebEngineProfile::defaultProfile();
        profile->setUrlRequestInterceptor(&blocker);
        struct ResetInterceptor {
            QWebEngineProfile* profile;
            ~ResetInterceptor() { profile->setUrlRequestInterceptor(nullptr); }
        } reset{profile};
        QFile config(isolatedConfig_.filePath("widgets.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(R"({"version":1,"ai":false,"tencentMap":{
            "webServiceKey":"file-api","jsKey":"widget-js","secretKey":"file-sk"}})");
        config.close();
        qputenv("CHARGING_SERVICES_CONFIG", config.fileName().toUtf8());
        charging::client::pages::station::StationMapPanel panel;
        QPointer<QWebEngineView> view = panel.findChild<QWebEngineView*>();
        QVERIFY(view);
        QSignalSpy loaded(view.data(), &QWebEngineView::loadFinished);
        QTRY_VERIFY(!loaded.isEmpty());
        QVERIFY(loaded.first().first().toBool());
        QVERIFY(view);
        QString html;
        view->page()->toHtml([&](const QString& result) { html = result; });
        QTRY_VERIFY(!html.isEmpty());
        QVERIFY(html.contains("key=widget-js"));
        QVERIFY(!html.contains("file-api"));
        QVERIFY(!html.contains("file-sk"));
    }
#endif
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
    QVERIFY(panel.findChild<QLabel*>("mapDegradedLabel")->toolTip().contains("config/services.local.json"));
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
