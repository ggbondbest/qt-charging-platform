#include "fake_tencent_server.h"
#include "map_bridge.h"

#include <QSignalSpy>
#include <QtTest>

using charging::qml::MapBridge;
using charging::testing::FakeTencentServer;
using namespace charging::client::services::map;

class QmlMapBridgeTest final : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        qputenv("TENCENT_MAP_API_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_KEY", "");
        qputenv("TENCENT_MAP_JS_KEY", "");
        qputenv("TENCENT_MAP_SECRET_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_SECRET", "");
    }
    void cleanup() { init(); }

    void requiresExplicitOriginAndNeverInventsRoute()
    {
        MapBridge bridge;
        QVERIFY(!bridge.hasLocation());
        QCOMPARE(bridge.distanceMeters(22.54, 113.94), -1);
        bridge.requestRoute(22.54, 113.94);
        QVERIFY(!bridge.error().isEmpty());
        QVERIFY(bridge.routeHtml().isEmpty());
        QCOMPARE(bridge.routeDistanceMeters(), -1);
        bridge.setUserLocation(22.54, 113.94);
        bridge.requestRoute(22.55, 113.95);
        QTRY_VERIFY(!bridge.busy());
        QVERIFY(bridge.error().contains(QStringLiteral("密钥")));
        QVERIFY(bridge.routeHtml().isEmpty());
        QVERIFY(bridge.steps().isEmpty());
    }

    void distanceComesFromCoordinates()
    {
        MapBridge bridge;
        bridge.setUserLocation(0, 0);
        QCOMPARE(bridge.distanceMeters(0, 0), 0);
        QVERIFY(qAbs(bridge.distanceMeters(0, 1) - 111195) <= 1);
        QVERIFY(bridge.distanceMeters(0, 0.1) < bridge.distanceMeters(0, 1));
        QCOMPARE(bridge.distanceMeters(91, 0), -1);
        bridge.setUserLocation(91, 0);
        QCOMPARE(bridge.latitude(), 0.0); // invalid input cannot replace the origin
    }

    void latestGeocodeOwnsLocation()
    {
        qputenv("TENCENT_MAP_API_KEY", "test-only");
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setHoldRequests(true);
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        MapBridge bridge(&service, nullptr);
        bridge.geocodeAddress(QStringLiteral("旧地址"));
        bridge.geocodeAddress(QStringLiteral("新地址"));
        service.forwardGeocodeSucceeded(1, {22.5, 113.9}, {});
        QVERIFY(!bridge.hasLocation());
        service.forwardGeocodeSucceeded(2, {31.2, 121.5}, {});
        QVERIFY(bridge.hasLocation());
        QCOMPARE(bridge.latitude(), 31.2);
        QCOMPARE(bridge.locationLabel(), QStringLiteral("新地址"));
    }

    void drivingWalkingAndCancellationUseRealRoute()
    {
        qputenv("TENCENT_MAP_API_KEY", "test-only");
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setJsonResponse(R"({"status":0,"result":{"routes":[{"distance":1234,
            "duration":19,"polyline":[22.541,113.943,1000,2000],
            "steps":[{"instruction":"沿道路步行","distance":1234}]}]}})");
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        MapBridge bridge(&service, nullptr);
        bridge.setUserLocation(22.541, 113.943);
        bridge.requestRoute(22.542, 113.945, QStringLiteral("walking"));
        QTRY_VERIFY(!bridge.busy());
        QVERIFY(bridge.error().isEmpty());
        QCOMPARE(bridge.routeDistanceMeters(), 1234);
        QCOMPARE(bridge.durationMinutes(), 19);
        QCOMPARE(bridge.steps().size(), 1);
        QVERIFY(bridge.routeHtml().contains(QStringLiteral("qq.maps.Polyline")));
        QVERIFY(server.lastRequestTarget().startsWith("/ws/direction/v1/walking/"));
        bridge.requestRoute(22.542, 113.945, QStringLiteral("driving"));
        QVERIFY(bridge.routeHtml().isEmpty());
        bridge.cancelRoute();
        QTest::qWait(30);
        QVERIFY(bridge.routeHtml().isEmpty());
        QCOMPARE(bridge.routeDistanceMeters(), -1);
    }

    void missingPolylineCannotProduceFakeLine()
    {
        qputenv("TENCENT_MAP_API_KEY", "test-only");
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setJsonResponse(R"({"status":0,"result":{"routes":[{"distance":1000,"duration":2}]}})");
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        MapBridge bridge(&service, nullptr);
        bridge.setUserLocation(22.541, 113.943);
        bridge.requestRoute(22.542, 113.945);
        QTRY_VERIFY(!bridge.busy());
        QVERIFY(!bridge.error().isEmpty());
        QVERIFY(bridge.routeHtml().isEmpty());
        QCOMPARE(bridge.routeDistanceMeters(), -1);
    }

    void htmlEscapesUntrustedStationName()
    {
        qputenv("TENCENT_MAP_JS_KEY", "test-only");
        MapBridge bridge;
        bridge.setUserLocation(22.54, 113.94);
        const auto html = bridge.mapHtml({QVariantMap{{"lat", 22.55}, {"lng", 113.95},
            {"label", "</script><script>alert('bad')</script>"}}});
        QVERIFY(!html.contains("</script><script>alert"));
        QVERIFY(html.contains("\\u003c/script\\u003e"));
    }
};

QTEST_GUILESS_MAIN(QmlMapBridgeTest)
#include "tst_qml_map_bridge.moc"
