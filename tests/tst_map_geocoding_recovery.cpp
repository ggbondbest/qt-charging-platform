#include "fake_tencent_server.h"
#include "map_bridge.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>

using namespace charging::client::services::map;
using charging::qml::MapBridge;
using charging::testing::FakeTencentServer;

namespace {
const QByteArray kDalian = R"({"status":0,"result":{"location":{"lat":38.88,"lng":121.53}}})";
const QByteArray kBeijing = R"({"status":0,"result":{"location":{"lat":39.9,"lng":116.4}}})";
}

class MapGeocodingRecoveryTest final : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        // Never load real credentials/config or contact Tencent.
        qputenv("TENCENT_MAP_API_KEY", "test-only-key");
        qputenv("CHARGING_TENCENT_MAP_KEY", "");
        qputenv("TENCENT_MAP_SECRET_KEY", "test-only-secret");
        qputenv("CHARGING_TENCENT_MAP_SECRET", "");
    }
    void cleanup()
    {
        qputenv("TENCENT_MAP_API_KEY", "");
        qputenv("TENCENT_MAP_SECRET_KEY", "");
    }

    void successFailureCachedOriginThenNewOrigin()
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        MapBridge bridge(&service, nullptr);
        server.setJsonResponse(kDalian);
        bridge.geocodeAddress(QStringLiteral("大连市 软件园路"));
        QTRY_VERIFY(!bridge.busy());
        QVERIFY(bridge.error().isEmpty());
        QCOMPARE(bridge.latitude(), 38.88);
        server.setJsonResponse(R"({"status":121,"message":"test-only-key"})");
        bridge.geocodeAddress(QStringLiteral("北京市 天安门广场"));
        QTRY_VERIFY(!bridge.busy());
        QVERIFY(bridge.error().contains(QStringLiteral("status 121")));
        QVERIFY(!bridge.error().contains(QStringLiteral("test-only")));
        QCOMPARE(bridge.latitude(), 38.88); // no invented new origin after failure
        QCOMPARE(bridge.locationLabel(), QStringLiteral("大连市 软件园路"));
        QCOMPARE(server.requestTargets().size(), 2);
        bridge.geocodeAddress(QStringLiteral("大连市 软件园路"));
        QVERIFY(bridge.busy()); // cached result is asynchronous too
        QTRY_VERIFY(!bridge.busy());
        QVERIFY(bridge.error().isEmpty());
        QCOMPARE(server.requestTargets().size(), 2);
        server.setJsonResponse(kBeijing);
        bridge.geocodeAddress(QStringLiteral("北京市 天安门广场"));
        QTRY_VERIFY(!bridge.busy());
        QVERIFY(bridge.error().isEmpty());
        QCOMPARE(bridge.latitude(), 39.9);
        QCOMPARE(bridge.locationLabel(), QStringLiteral("北京市 天安门广场"));
        QCOMPARE(server.requestTargets().size(), 3); // failures are never cached
    }

    void bothInterfacesCoalesceAndCacheAsynchronously()
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setHoldRequests(true);
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy native(&service, &MapGeoService::forwardGeocodeSucceeded);
        QSignalSpy qml(&service, &MapGeoService::qmlGeocodeReady);
        const auto a = service.requestForwardGeocode(QStringLiteral("大连"));
        const auto b = service.requestAddressGeocode(QStringLiteral(" 大连 "));
        const auto c = service.requestForwardGeocode(QStringLiteral("大连"));
        QVERIFY(a < b && b < c);
        QTRY_COMPARE(server.requestTargets().size(), 1);
        server.releasePending(kDalian);
        QTRY_COMPARE(native.size(), 2);
        QCOMPARE(qml.size(), 1);
        QCOMPARE(native.at(0).at(0).toULongLong(), a);
        QCOMPARE(native.at(1).at(0).toULongLong(), c);
        QCOMPARE(qml.at(0).at(0).toULongLong(), b);
        const auto d = service.requestAddressGeocode(QStringLiteral("大连"));
        QCOMPARE(qml.size(), 1);
        QTRY_COMPARE(qml.size(), 2);
        QCOMPARE(qml.at(1).at(0).toULongLong(), d);
        QCOMPARE(qml.at(1).at(1).toMap().value("latitude").toDouble(), 38.88);
        QCOMPARE(server.requestTargets().size(), 1);
    }

    void temporaryRateLimitRetriesOnceAfterDelay()
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setJsonResponse(R"({"status":120})");
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        MapBridge bridge(&service, nullptr);
        QElapsedTimer elapsed;
        elapsed.start();
        bridge.geocodeAddress(QStringLiteral("北京"));
        QTRY_COMPARE(server.requestTargets().size(), 1);
        server.setJsonResponse(kBeijing); // first response already copied by stub
        QTRY_VERIFY_WITH_TIMEOUT(!bridge.busy(), 3000);
        QVERIFY(bridge.error().isEmpty());
        QCOMPARE(bridge.latitude(), 39.9);
        QCOMPARE(server.requestTargets().size(), 2);
        QVERIFY(elapsed.elapsed() >= 1000);
    }

    void specificErrorsAndBoundedRetries_data()
    {
        QTest::addColumn<int>("http");
        QTest::addColumn<int>("status");
        QTest::addColumn<MapError>("expected");
        QTest::addColumn<int>("attempts");
        QTest::newRow("quota") << 200 << 121 << MapError::QuotaExhausted << 1;
        QTest::newRow("403-quota") << 403 << 121 << MapError::QuotaExhausted << 1;
        QTest::newRow("403-signature") << 403 << 111 << MapError::InvalidKey << 1;
        QTest::newRow("403-unknown") << 403 << -1 << MapError::AccessDenied << 1;
        QTest::newRow("401") << 401 << -1 << MapError::InvalidKey << 1;
        QTest::newRow("qps") << 200 << 120 << MapError::RateLimited << 2;
        QTest::newRow("429") << 429 << -1 << MapError::RateLimited << 2;
    }
    void specificErrorsAndBoundedRetries()
    {
        QFETCH(int, http); QFETCH(int, status);
        QFETCH(MapError, expected); QFETCH(int, attempts);
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setResponse(http, QByteArray("{\"status\":") + QByteArray::number(status)
            + ",\"message\":\"test-only-key test-only-secret &sig=sensitive\"}");
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy failed(&service, &MapGeoService::forwardGeocodeFailed);
        QSignalSpy qmlFailed(&service, &MapGeoService::qmlGeocodeError);
        service.requestForwardGeocode(QStringLiteral("北京"));
        service.requestAddressGeocode(QStringLiteral("北京"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
        QCOMPARE(qmlFailed.size(), 1);
        QCOMPARE(failed.first().at(1).value<MapError>(), expected);
        const QString message = failed.first().at(2).toString();
        QVERIFY(message.contains(QStringLiteral("HTTP %1").arg(http)));
        if (status > 0) QVERIFY(message.contains(QStringLiteral("status %1").arg(status)));
        QVERIFY(!message.contains(QStringLiteral("test-only")));
        QVERIFY(!message.contains(QStringLiteral("sig=")));
        QCOMPARE(qmlFailed.first().at(1).toString(), message);
        QTest::qWait(1200); // no hidden infinite retry after terminal failure
        QCOMPARE(server.requestTargets().size(), attempts);
        server.setJsonResponse(kBeijing);
        QSignalSpy ready(&service, &MapGeoService::qmlGeocodeReady);
        service.requestAddressGeocode(QStringLiteral("北京"));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(server.requestTargets().size(), attempts + 1);
    }

    void timeoutAndInvalidResponsesDoNotPoisonNextRequest()
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setHoldRequests(true);
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        service.setRequestTimeoutForTesting(80);
        QSignalSpy failed(&service, &MapGeoService::qmlGeocodeError);
        service.requestAddressGeocode(QStringLiteral("北京"));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(failed.first().at(1).toString().contains(QStringLiteral("超时")));
        server.setHoldRequests(false);
        service.setRequestTimeoutForTesting(2000);
        server.setJsonResponse(R"({"status":0,"result":{"location":{"lat":22.5}}})");
        service.requestAddressGeocode(QStringLiteral("北京"));
        QTRY_COMPARE(failed.size(), 2);
        service.requestAddressGeocode(QString(257, QLatin1Char('x')));
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(server.requestTargets().size(), 2);
        server.setJsonResponse(kBeijing);
        QSignalSpy ready(&service, &MapGeoService::qmlGeocodeReady);
        service.requestAddressGeocode(QStringLiteral("北京"));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(server.requestTargets().size(), 3);
    }

    void longRetryAfterIsNotRetriedEarly()
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setResponse(429, "{}");
        server.setRetryAfter("30");
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy failed(&service, &MapGeoService::qmlGeocodeError);
        service.requestAddressGeocode(QStringLiteral("北京"));
        QTRY_COMPARE(failed.size(), 1);
        QTest::qWait(1200);
        QCOMPARE(server.requestTargets().size(), 1);
    }

    void cacheIsBoundedAndDoesNotMixDifferentAddresses()
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setJsonResponse(kDalian);
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy ready(&service, &MapGeoService::qmlGeocodeReady);
        for (int i = 0; i < 33; ++i) {
            service.requestAddressGeocode(QStringLiteral("地址%1").arg(i));
            QTRY_COMPARE(ready.size(), i + 1);
        }
        QCOMPARE(server.requestTargets().size(), 33);
        server.setJsonResponse(kBeijing);
        service.requestAddressGeocode(QStringLiteral("地址0")); // evicted, must re-query
        QTRY_COMPARE(ready.size(), 34);
        QCOMPARE(server.requestTargets().size(), 34);
        QCOMPARE(ready.last().at(1).toMap().value("latitude").toDouble(), 39.9);
        service.requestAddressGeocode(QStringLiteral("地址32")); // still cached
        QTRY_COMPARE(ready.size(), 35);
        QCOMPARE(server.requestTargets().size(), 34);
        QCOMPARE(ready.last().at(1).toMap().value("latitude").toDouble(), 38.88);
    }
};

QTEST_GUILESS_MAIN(MapGeocodingRecoveryTest)
#include "tst_map_geocoding_recovery.moc"
