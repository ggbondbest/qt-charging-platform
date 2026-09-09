// 腾讯地图 WebService 封装（MapGeoService）单测：全程走进程内假 HTTP 服务
// （fake_tencent_server.h），永不触真实网络；CI 无 key 也全绿。
#include "fake_tencent_server.h"
#include "services/map/map_geo_service.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

using namespace charging::client::services::map;
using charging::testing::FakeTencentServer;

namespace {

const QByteArray kMatrixJson = R"({
    "status": 0,
    "message": "query ok",
    "request_id": "unit-test",
    "result": {"rows": [{"elements": [
        {"distance": 4321, "duration": 600, "text": ""},
        {"distance": 900, "duration": 180, "text": ""}
    ]}]}
})";

// 真实响应结构（curl 实测）：路线在 result.routes[0]，duration 单位=分钟。
// polyline 为增量压缩口径：[lat,lng] 成对、每对相对上一点增量 ×1e6（首点绝对）。
const QByteArray kRouteJson = R"({
    "status": 0,
    "message": "Success",
    "result": {"routes": [{
        "mode": "DRIVING",
        "distance": 5120,
        "duration": 12,
        "traffic_light_count": 3,
        "polyline": [22541000, 113943000, 1000, 2000, 500, -3000],
        "steps": [
            {"instruction": "沿滨海大道直行约2000米", "distance": 2000, "duration": 480},
            {"instruction": "在路口右转进入科苑北路", "distance": 800, "duration": 180},
            {"instruction": "到达目的地附近", "distance": 10, "duration": 5}
        ]}]}
})";

// 旧文档口径（result.mode）：解析器保留兼容回退。
const QByteArray kRouteLegacyJson = R"({
    "status": 0,
    "result": {"mode": {"distance": 3000, "duration": 8,
        "steps": [{"instruction": "直行", "distance": 3000}]}}
})";

const QByteArray kGeocodeJson = R"({
    "status": 0,
    "message": "Success",
    "result": {"location": {"lat": 22.541, "lng": 113.943},
        "address": "广东省深圳市南山区科兴路",
        "address_component": {"city": "深圳市", "district": "南山区"}}
})";

} // namespace

class MapGeoServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        // 默认无 key 无 SK（用例内按需覆盖，cleanup 统一清除，防泄漏）。
        // 两个环境变量名都要清：新名 TENCENT_MAP_API_KEY + 旧兼容名。
        qputenv("TENCENT_MAP_API_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_KEY", "");
        qputenv("TENCENT_MAP_SECRET_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_SECRET", "");
    }
    void cleanup()
    {
        qputenv("TENCENT_MAP_API_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_KEY", "");
        qputenv("TENCENT_MAP_SECRET_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_SECRET", "");
    }

    void noKeyFailsAsyncWithoutAnyNetwork();
    void envPrefersNewNameOverLegacy();
    // Local configuration files must never supply credentials or redirect their requests.
    void configFileCannotSupplyKeyOrEndpoint();
    void environmentIsOnlyProductionConfiguration();
    void matrixParsesMultipleDestinations();
    void requestQueryMatchesTencentContract();
    void businessStatusMapsToTypedErrors();
    void transportFailuresMapToTypedErrors();
    void routeParsesDistanceDurationAndSteps();
    void routePolylineOutOfRegionIsIgnored();
    void routePolylineAcceptsDegreesFirstPoint();
    void routeFallsBackToLegacyModeShape();
    void geocodeParsesAddress();
    void requestIdsAreDistinctAndAscending();
    // —— QML 转发面（导航页"地图 APP"交互消费） ——
    void qmlSurfaceNoKeyFailsAsync();
    void ipLocationRelayParsesCityPoint();
    void addressGeocodeEncodesChineseAndRelays();
    void routeRelayCarriesQmlFriendlyMap();
    void staticMapWritesPngAndRotatesFile();
    void staticMapDownsamplesLongPolyline();
    void staticMapJsonBodyClassifiesError();
    void navigationUriUrlShapeAndEncoding();
    void forwardGeocodeUsesAddressAndReturnsCoordinates();
    void forwardGeocodeRejectsMissingCoordinates();
    void walkingUsesDedicatedEndpoint();
    void invalidCoordinatesDoNotReachNetwork();
    void routeRoundsFractionalMinutesUp();
};

void MapGeoServiceTest::noKeyFailsAsyncWithoutAnyNetwork()
{
    FakeTencentServer server;
    QVERIFY(server.start());

    MapGeoService service;
    QVERIFY(!service.hasUsableKey());
    QCOMPARE(service.userLocation().latitude, 22.541); // 演示中心坐标口径
    QCOMPARE(service.userLocation().longitude, 113.943);

    QSignalSpy matrixFailed(&service, &MapGeoService::distanceMatrixFailed);
    QSignalSpy routeFailed(&service, &MapGeoService::routeFailed);
    const quint64 id = service.requestDistanceMatrix({{22.55, 113.95}});
    QVERIFY(id > 0);
    QVERIFY(matrixFailed.wait(2000));
    QCOMPARE(matrixFailed.at(0).at(0).toULongLong(), id);
    QCOMPARE(matrixFailed.at(0).at(1).value<MapError>(), MapError::NoApiKey);
    QCOMPARE(matrixFailed.at(0).at(2).toString(), QStringLiteral("未配置地图密钥"));

    const quint64 routeId = service.requestDrivingRoute({22.541, 113.943}, {22.55, 113.95});
    QVERIFY(routeId > 0);
    QVERIFY(routeFailed.wait(2000));
    QCOMPARE(routeFailed.at(0).at(1).value<MapError>(), MapError::NoApiKey);
    // 零网络触达 = 无 key 时页面行为与接入前逐字节一致的前提。
    QCOMPARE(server.connectionCount(), 0);
}

void MapGeoServiceTest::envPrefersNewNameOverLegacy()
{
    // 任务书口径：TENCENT_MAP_API_KEY 优先；旧名仅作兼容回退。
    qputenv("TENCENT_MAP_API_KEY", "new-name-key");
    qputenv("CHARGING_TENCENT_MAP_KEY", "legacy-name-key");
    QCOMPARE(MapGeoService::apiKeyFromEnvironment(), QStringLiteral("new-name-key"));

    qputenv("TENCENT_MAP_API_KEY", "");
    QCOMPARE(MapGeoService::apiKeyFromEnvironment(), QStringLiteral("legacy-name-key"));

    qputenv("CHARGING_TENCENT_MAP_KEY", "");
    QVERIFY(MapGeoService::apiKeyFromEnvironment().isEmpty());
}

void MapGeoServiceTest::configFileCannotSupplyKeyOrEndpoint()
{
    // Even undefined environment variables do not enable a credential-file fallback.
    qunsetenv("TENCENT_MAP_API_KEY");
    qunsetenv("CHARGING_TENCENT_MAP_KEY");
    QVERIFY(!MapGeoService::environmentKeyAuthoritative());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("map_services.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(R"({"tencentMapKey":"cfg-file-key","baseUrl":"https://example.test/ws/"})");
    f.close();

    QVERIFY(MapGeoService::apiKeyFromConfigFile(path).isEmpty());
    QVERIFY(MapGeoService::baseUrlFromConfigFile(path).isEmpty());
    QVERIFY(MapGeoService::resolveApiKey(path).isEmpty());
    QVERIFY(MapGeoService::resolveApiKey().isEmpty());
    QCOMPARE(MapGeoService::resolveBaseUrl(path), QStringLiteral("https://apis.map.qq.com/ws"));
    QCOMPARE(MapGeoService::resolveBaseUrl(), QStringLiteral("https://apis.map.qq.com/ws"));
    QVERIFY(MapGeoService::apiKeyFromConfigFile(dir.filePath("missing.json")).isEmpty());

    FakeTencentServer server;
    QVERIFY(server.start());
    MapGeoService service;
    QVERIFY(!service.hasUsableKey());
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy failed(&service, &MapGeoService::forwardGeocodeFailed);
    const quint64 requestId = service.requestForwardGeocode(QStringLiteral("大连市 软件园路"));
    QCOMPARE(failed.size(), 0); // no-key failure remains asynchronous
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed.first().at(0).toULongLong(), requestId);
    QCOMPARE(failed.first().at(1).value<MapError>(), MapError::NoApiKey);
    QCOMPARE(server.connectionCount(), 0);
}

void MapGeoServiceTest::environmentIsOnlyProductionConfiguration()
{
    // File contents are ignored both with empty and nonempty environment keys.
    qputenv("TENCENT_MAP_API_KEY", "");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("map_services.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(R"({"tencentMapKey":"cfg-file-key","baseUrl":"https://example.test/ws"})");
    f.close();

    QVERIFY(MapGeoService::environmentKeyAuthoritative());
    QVERIFY(MapGeoService::resolveApiKey(path).isEmpty());
    QVERIFY(!MapGeoService{}.hasUsableKey());

    qputenv("CHARGING_TENCENT_MAP_KEY", "env-wins");
    QCOMPARE(MapGeoService::resolveApiKey(path), QStringLiteral("env-wins"));
    qputenv("TENCENT_MAP_API_KEY", "preferred-env-key");
    QCOMPARE(MapGeoService::resolveApiKey(path), QStringLiteral("preferred-env-key"));
    QVERIFY(MapGeoService::apiKeyFromConfigFile(path).isEmpty());
    QVERIFY(MapGeoService::baseUrlFromConfigFile(path).isEmpty());
    QCOMPARE(MapGeoService::resolveBaseUrl(path), QStringLiteral("https://apis.map.qq.com/ws"));
}

void MapGeoServiceTest::matrixParsesMultipleDestinations()
{
    qputenv("CHARGING_TENCENT_MAP_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kMatrixJson);

    MapGeoService service;
    QVERIFY(service.hasUsableKey());
    service.setEndpointBaseForTesting(server.endpointBase());

    QSignalSpy succeeded(&service, &MapGeoService::distanceMatrixSucceeded);
    QSignalSpy failed(&service, &MapGeoService::distanceMatrixFailed);
    const quint64 id = service.requestDistanceMatrix({{22.55, 113.95}, {22.56, 113.96}});
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);

    QVERIFY2(succeeded.count() == 1, qPrintable(failed.count()
                ? failed.at(0).at(2).toString()
                : QStringLiteral("no response")));
    QCOMPARE(succeeded.at(0).at(0).toULongLong(), id);
    const auto elements = succeeded.at(0).at(1).value<QVector<DistanceElement>>();
    QCOMPARE(elements.size(), 2);
    QCOMPARE(elements.at(0).distanceMeters, 4321);
    QCOMPARE(elements.at(0).durationSeconds, 600); // 矩阵时长口径：秒
    QCOMPARE(elements.at(1).distanceMeters, 900);
    QCOMPARE(elements.at(1).durationSeconds, 180);
}

void MapGeoServiceTest::requestQueryMatchesTencentContract()
{
    qputenv("CHARGING_TENCENT_MAP_KEY", "unit-test-key");
    qputenv("CHARGING_TENCENT_MAP_SECRET", "unit-sk");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kMatrixJson);

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::distanceMatrixSucceeded);
    QSignalSpy failed(&service, &MapGeoService::distanceMatrixFailed);
    service.requestDistanceMatrix({{22.55, 113.95}});
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QCOMPARE(succeeded.count(), 1);

    const QString target = server.lastRequestTarget();
    QVERIFY2(target.startsWith(QStringLiteral("/ws/distance/v1/matrix/")), qPrintable(target));
    QVERIFY(target.contains(QStringLiteral("mode=driving")));
    QVERIFY(target.contains(QStringLiteral("from=22.541000,113.943000"))); // 6 位小数口径
    QVERIFY(target.contains(QStringLiteral("to=22.550000,113.950000")));
    QVERIFY(target.contains(QStringLiteral("key=unit-test-key")));

    // 官方签名规则：sig = MD5小写(path + "?" + 参数按 key 升序拼接 + SK)。
    const QByteArray expectedRaw = QByteArrayLiteral(
        "/ws/distance/v1/matrix/?from=22.541000,113.943000&key=unit-test-key"
        "&mode=driving&to=22.550000,113.950000unit-sk");
    const QByteArray expectedSig =
        QCryptographicHash::hash(expectedRaw, QCryptographicHash::Md5).toHex();
    QVERIFY2(target.contains(QStringLiteral("sig=") + QString::fromLatin1(expectedSig)),
             qPrintable(target));
    // key 绝不出现在信号错误文案里（本用例成功路径无文案，此处保护口径注释）。
}

void MapGeoServiceTest::forwardGeocodeUsesAddressAndReturnsCoordinates()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kGeocodeJson);
    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::forwardGeocodeSucceeded);
    const QString address = QStringLiteral("深圳市南山区 A&B 大厦 #2");
    const quint64 id = service.requestForwardGeocode(address);
    QTRY_COMPARE(succeeded.size(), 1);
    QCOMPARE(succeeded.first().at(0).toULongLong(), id);
    const auto location = succeeded.first().at(1).value<LatLng>();
    QCOMPARE(location.latitude, 22.541);
    QCOMPARE(location.longitude, 113.943);
    const QUrl url(QStringLiteral("http://localhost") + server.lastRequestTarget());
    QCOMPARE(url.path(), QStringLiteral("/ws/geocoder/v1/"));
    QCOMPARE(QUrlQuery(url).queryItemValue("address", QUrl::FullyDecoded), address);
    QCOMPARE(QUrlQuery(url).queryItemValue("key"), QStringLiteral("unit-test-key"));
    QVERIFY(!QUrlQuery(url).hasQueryItem("location"));
}

void MapGeoServiceTest::forwardGeocodeRejectsMissingCoordinates()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(R"({"status":0,"result":{"location":{"lat":22.5}}})");
    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy failed(&service, &MapGeoService::forwardGeocodeFailed);
    service.requestForwardGeocode(QStringLiteral("深圳市南山区"));
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed.first().at(1).value<MapError>(), MapError::BadResponse);
}

void MapGeoServiceTest::walkingUsesDedicatedEndpoint()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kRouteJson);
    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::routeSucceeded);
    service.requestWalkingRoute({22.541, 113.943}, {22.55, 113.95});
    QTRY_COMPARE(succeeded.size(), 1);
    QVERIFY(server.lastRequestTarget().startsWith("/ws/direction/v1/walking/"));
    const auto route = succeeded.first().at(1).value<RouteResult>();
    QCOMPARE(route.durationMinutes, 12);
    QCOMPARE(route.distanceMeters, 5120);
}

void MapGeoServiceTest::invalidCoordinatesDoNotReachNetwork()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy failed(&service, &MapGeoService::routeFailed);
    service.requestWalkingRoute({91, 0}, {22.55, 113.95});
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(server.connectionCount(), 0);
}

void MapGeoServiceTest::routeRoundsFractionalMinutesUp()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(R"({"status":0,"result":{"routes":[{"distance":800,
        "duration":1.5,"polyline":[22.541,113.943,1000,1000]}]}})");
    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::routeSucceeded);
    service.requestDrivingRoute({22.541, 113.943}, {22.542, 113.944});
    QTRY_COMPARE(succeeded.size(), 1);
    QCOMPARE(succeeded.first().at(1).value<RouteResult>().durationMinutes, 2);
}

void MapGeoServiceTest::businessStatusMapsToTypedErrors()
{
    struct { int status; MapError expected; } cases[] = {
        {121, MapError::QuotaExhausted}, // 每日配额超限，不得自动重试
        {120, MapError::RateLimited}, // 并发限制
        {310, MapError::InvalidKey},  // 密钥无效
        {312, MapError::InvalidKey},  // 无接口权限
        {999, MapError::BadResponse}, // 其它业务错误
    };
    qputenv("CHARGING_TENCENT_MAP_KEY", "unit-test-key");
    for (const auto& testCase : cases) {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setJsonResponse(QByteArray("{\"status\": ") + QByteArray::number(testCase.status)
            + ", \"message\": \"业务错误，不透明转发\"}");

        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy failed(&service, &MapGeoService::distanceMatrixFailed);
        service.requestDistanceMatrix({{22.55, 113.95}});
        QVERIFY2(failed.wait(5000), QByteArray::number(testCase.status));
        QCOMPARE(failed.at(0).at(1).value<MapError>(), testCase.expected);
        // 腾讯 message 不透传（可能含 key 相关提示），只给固定分类文案。
        QVERIFY(!failed.at(0).at(2).toString().contains(QStringLiteral("不透传")));
    }
}

void MapGeoServiceTest::transportFailuresMapToTypedErrors()
{
    qputenv("CHARGING_TENCENT_MAP_KEY", "unit-test-key");

    // 坏 JSON → BadResponse
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setResponse(200, QByteArray("这不是JSON{{{"));
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy failed(&service, &MapGeoService::distanceMatrixFailed);
        service.requestDistanceMatrix({{22.55, 113.95}});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(1).value<MapError>(), MapError::BadResponse);
    }
    // HTTP 403 无业务原因不能猜成配额或限流。
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setResponse(403, QByteArray("{}"));
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy failed(&service, &MapGeoService::distanceMatrixFailed);
        service.requestDistanceMatrix({{22.55, 113.95}});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(1).value<MapError>(), MapError::AccessDenied);
    }
    // 静默不回包 → Timeout（压缩超时）
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setHoldRequests(true);
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        service.setRequestTimeoutForTesting(80);
        QSignalSpy failed(&service, &MapGeoService::distanceMatrixFailed);
        service.requestDistanceMatrix({{22.55, 113.95}});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(1).value<MapError>(), MapError::Timeout);
    }
    // 无人监听端口 → Network（拒连/断网兜底路径）
    {
        const quint16 deadPort = FakeTencentServer::closedPort();
        MapGeoService service;
        service.setEndpointBaseForTesting(
            QStringLiteral("http://127.0.0.1:%1/ws").arg(deadPort));
        QSignalSpy failed(&service, &MapGeoService::routeFailed);
        service.requestDrivingRoute({22.541, 113.943}, {22.55, 113.95});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(1).value<MapError>(), MapError::Network);
    }
}

void MapGeoServiceTest::routeParsesDistanceDurationAndSteps()
{
    qputenv("CHARGING_TENCENT_MAP_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kRouteJson);

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::routeSucceeded);
    QSignalSpy failed(&service, &MapGeoService::routeFailed);
    const quint64 id = service.requestDrivingRoute({22.541, 113.943}, {22.55, 113.95});
    QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() + failed.count() > 0, 5000);
    QVERIFY2(succeeded.count() == 1,
             failed.count() ? qPrintable(failed.at(0).at(2).toString())
                            : "no route response");

    QCOMPARE(succeeded.at(0).at(0).toULongLong(), id);
    const RouteResult route = succeeded.at(0).at(1).value<RouteResult>();
    QCOMPARE(route.distanceMeters, 5120);
    QCOMPARE(route.durationMinutes, 12); // 路线时长口径：分钟（与矩阵的秒区分）
    QCOMPARE(route.steps.size(), 3);
    QCOMPARE(route.steps.at(0).instruction, QStringLiteral("沿滨海大道直行约2000米"));
    QCOMPARE(route.steps.at(1).distanceMeters, 800);

    // 增量解码：首点绝对/1e6，其后逐对累加。
    QCOMPARE(route.polyline.size(), 3);
    QVERIFY(qAbs(route.polyline.at(0).latitude - 22.541) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(0).longitude - 113.943) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(1).latitude - 22.542) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(1).longitude - 113.945) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(2).latitude - 22.5425) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(2).longitude - 113.942) < 1e-9);

    QVERIFY(server.lastRequestTarget().startsWith(QStringLiteral("/ws/direction/v1/driving/")));
    QVERIFY(server.lastRequestTarget().contains(QStringLiteral("to=22.550000,113.950000")));
}

void MapGeoServiceTest::routePolylineAcceptsDegreesFirstPoint()
{
    // 官方 JS 示例口径（guide-polyline）：前两个元素是首点绝对**度数**，
    // 索引 2 起为整数微度增量：coors[i] = coors[i-2] + coors[i]/1e6。
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(QByteArrayLiteral(R"({"status": 0, "result": {"routes": [{
        "distance": 1000, "duration": 3,
        "polyline": [22.541000, 113.943000, -345, -1828, 19867, -26154]}]}})"));

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::routeSucceeded);
    service.requestDrivingRoute({22.541, 113.943}, {22.56, 113.95});
    QVERIFY(succeeded.wait(5000));
    const RouteResult route = succeeded.at(0).at(1).value<RouteResult>();
    QCOMPARE(route.polyline.size(), 3);
    QVERIFY(qAbs(route.polyline.at(0).latitude - 22.541) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(0).longitude - 113.943) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(1).latitude - (22.541 - 0.000345)) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(1).longitude - (113.943 - 0.001828)) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(2).latitude - (22.541 - 0.000345 + 0.019867)) < 1e-9);
    QVERIFY(qAbs(route.polyline.at(2).longitude - (113.943 - 0.001828 - 0.026154)) < 1e-9);
}

void MapGeoServiceTest::routeFallsBackToLegacyModeShape()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kRouteLegacyJson);

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::routeSucceeded);
    service.requestDrivingRoute({22.541, 113.943}, {22.55, 113.95});
    QVERIFY(succeeded.wait(5000));
    const RouteResult route = succeeded.at(0).at(1).value<RouteResult>();
    QCOMPARE(route.distanceMeters, 3000);
    QCOMPARE(route.durationMinutes, 8);
    QCOMPARE(route.steps.size(), 1);
    QVERIFY(route.polyline.isEmpty()); // 无 polyline 字段：空折线（消费方回落模拟）
}

void MapGeoServiceTest::routePolylineOutOfRegionIsIgnored()
{
    // 解码越界（口径变化/脏数据）时折线整体置空而非画出飞线，页面回落模拟。
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(QByteArrayLiteral(R"({"status": 0, "result": {"routes": [{
        "distance": 100, "duration": 1, "polyline": [999000000, 0]}]}})"));

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy succeeded(&service, &MapGeoService::routeSucceeded);
    service.requestDrivingRoute({22.541, 113.943}, {22.55, 113.95});
    QVERIFY(succeeded.wait(5000));
    QVERIFY(succeeded.at(0).at(1).value<RouteResult>().polyline.isEmpty());
}

void MapGeoServiceTest::geocodeParsesAddress()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());

    // 成功：result.address 直出。
    server.setJsonResponse(kGeocodeJson);
    QSignalSpy succeeded(&service, &MapGeoService::geocodeSucceeded);
    QSignalSpy failed(&service, &MapGeoService::geocodeFailed);
    const quint64 id = service.requestReverseGeocode({22.541, 113.943});
    QVERIFY(succeeded.wait(5000));
    QCOMPARE(succeeded.at(0).at(0).toULongLong(), id);
    QCOMPARE(succeeded.at(0).at(1).toString(), QStringLiteral("广东省深圳市南山区科兴路"));
    QVERIFY(server.lastRequestTarget().startsWith(QStringLiteral("/ws/geocoder/v1/")));
    QVERIFY(server.lastRequestTarget().contains(QStringLiteral("location=22.541000,113.943000")));

    // 缺 address 字段 → BadResponse（消费方回落模拟地址，不崩不卡）。
    server.setJsonResponse(QByteArrayLiteral("{\"status\": 0, \"result\": {}}"));
    service.requestReverseGeocode({22.55, 113.95});
    QVERIFY(failed.wait(5000));
    QCOMPARE(failed.at(0).at(1).value<MapError>(), MapError::BadResponse);
}

void MapGeoServiceTest::requestIdsAreDistinctAndAscending()
{
    FakeTencentServer server;
    QVERIFY(server.start());
    MapGeoService service; // 无 key：NoApiKey 异步路径也要保序
    QSignalSpy matrixFailed(&service, &MapGeoService::distanceMatrixFailed);
    const quint64 first = service.requestDistanceMatrix({{22.55, 113.95}});
    const quint64 second = service.requestDrivingRoute({22.541, 113.943}, {22.55, 113.95});
    QVERIFY(first != second);
    QVERIFY(second > first);
    QTRY_COMPARE_WITH_TIMEOUT(matrixFailed.count(), 1, 2000);
    QCOMPARE(server.connectionCount(), 0);
}

// —— QML 转发面用例 ——

void MapGeoServiceTest::qmlSurfaceNoKeyFailsAsync()
{
    FakeTencentServer server;
    QVERIFY(server.start());
    MapGeoService service;
    QVERIFY(!service.usable());

    QSignalSpy ipError(&service, &MapGeoService::qmlIpLocationError);
    QSignalSpy geoError(&service, &MapGeoService::qmlGeocodeError);
    QSignalSpy staticError(&service, &MapGeoService::qmlStaticMapError);
    QSignalSpy routeError(&service, &MapGeoService::qmlRouteError);
    service.requestIpLocation();
    service.requestAddressGeocode(QStringLiteral("深圳市南山区科兴路"));
    service.requestStaticMap(22.541, 113.943, 12, 600, 400, {}, {});
    service.requestDrivingRoute(22.541, 113.943, 22.55, 113.95);   // QML 标量重载
    // 0ms singleShot 在同一个事件循环轮次全部发射：用 QTRY_COMPARE 收计数
    // （QSignalSpy::wait 只等"下一次"发射，先到先发的会把后面的吃掉）。
    QTRY_COMPARE_WITH_TIMEOUT(ipError.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(geoError.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(staticError.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(routeError.count(), 1, 2000);
    QCOMPARE(ipError.at(0).at(1).toString(), QStringLiteral("未配置地图密钥"));
    // 无 key = 零网络触达 + 跳转 URL 不可得（页面隐藏"跳转导航"按钮）。
    QCOMPARE(server.connectionCount(), 0);
    QVERIFY(service.navigationUriUrl(22.541, 113.943, QStringLiteral("起点"),
                                     22.55, 113.95, QStringLiteral("终点")).isEmpty());
}

void MapGeoServiceTest::ipLocationRelayParsesCityPoint()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(QByteArrayLiteral(R"({"status": 0, "result": {
        "location": {"lat": 22.5432, "lng": 113.9445},
        "ad_info": {"province": "广东省", "city": "深圳市", "district": "南山区"}}})"));

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy ready(&service, &MapGeoService::qmlIpLocationReady);
    QSignalSpy failed(&service, &MapGeoService::qmlIpLocationError);
    const quint64 id = service.requestIpLocation();
    QVERIFY(ready.wait(5000));
    QCOMPARE(ready.at(0).at(0).toULongLong(), id);
    const QVariantMap point = ready.at(0).at(1).toMap();
    QVERIFY(qAbs(point.value(QStringLiteral("latitude")).toDouble() - 22.5432) < 1e-9);
    QCOMPARE(point.value(QStringLiteral("city")).toString(), QStringLiteral("深圳市"));
    QVERIFY(server.lastRequestTarget().startsWith(QStringLiteral("/ws/location/v1/ip/")));

    // 标量读写面（导航页自动定位成功后写回）：
    service.setUserLocationLatLng(22.5432, 113.9445);
    const QVariantMap stored = service.userLocationMap();
    QVERIFY(qAbs(stored.value(QStringLiteral("longitude")).toDouble() - 113.9445) < 1e-9);

    // location 缺失 → qmlIpLocationError（页面自决"定位失败"文案）。
    server.setJsonResponse(QByteArrayLiteral("{\"status\": 0, \"result\": {}}"));
    service.requestIpLocation();
    QVERIFY(failed.wait(5000));
}

void MapGeoServiceTest::addressGeocodeEncodesChineseAndRelays()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kGeocodeJson);

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy ready(&service, &MapGeoService::qmlGeocodeReady);
    const quint64 id = service.requestAddressGeocode(QStringLiteral("深圳市南山区科兴路"));
    QVERIFY(ready.wait(5000));
    QCOMPARE(ready.at(0).at(0).toULongLong(), id);
    const QVariantMap point = ready.at(0).at(1).toMap();
    QVERIFY(qAbs(point.value(QStringLiteral("latitude")).toDouble() - 22.541) < 1e-9);
    QCOMPARE(point.value(QStringLiteral("address")).toString(),
             QStringLiteral("广东省深圳市南山区科兴路"));
    // 中文参数百分号编码（深=E6 B7 B1），path 口径正确。
    QVERIFY(server.lastRequestTarget().startsWith(QStringLiteral("/ws/geocoder/v1/")));
    QVERIFY2(server.lastRequestTarget().contains(QStringLiteral("address=%E6%B7%B1")),
             qPrintable(server.lastRequestTarget()));

    // 空文本不发请求，异步 BadResponse。
    QSignalSpy failed(&service, &MapGeoService::qmlGeocodeError);
    const int connectionsBefore = server.connectionCount();
    service.requestAddressGeocode(QStringLiteral("   "));
    QVERIFY(failed.wait(2000));
    QCOMPARE(server.connectionCount(), connectionsBefore);
}

void MapGeoServiceTest::routeRelayCarriesQmlFriendlyMap()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(kRouteJson);

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy relay(&service, &MapGeoService::qmlRouteReady);
    // QML 标量重载 → 与 C++ 原形同点旁路 QVariantMap 形。
    const quint64 id = service.requestDrivingRoute(22.541, 113.943, 22.55, 113.95);
    QVERIFY(relay.wait(5000));
    QCOMPARE(relay.at(0).at(0).toULongLong(), id);
    const QVariantMap route = relay.at(0).at(1).toMap();
    QCOMPARE(route.value(QStringLiteral("distanceMeters")).toInt(), 5120);
    QCOMPARE(route.value(QStringLiteral("durationMinutes")).toInt(), 12);
    const QVariantList polyline = route.value(QStringLiteral("polyline")).toList();
    QVERIFY2(polyline.size() == 3,
             qPrintable(QStringLiteral("relay=%1 first=%2")
                            .arg(relay.count())
                            .arg(polyline.isEmpty() ? QStringLiteral("(empty)")
                                                   : polyline.at(0).toString())));
    QCOMPARE(polyline.at(0).toList().size(), 2);
    QVERIFY(qAbs(polyline.at(0).toList().at(0).toDouble() - 22.541) < 1e-9);
    QVERIFY(qAbs(polyline.at(2).toList().at(1).toDouble() - 113.942) < 1e-9);
    const QVariantList steps = route.value(QStringLiteral("steps")).toList();
    QCOMPARE(steps.size(), 3);
    QCOMPARE(steps.at(0).toMap().value(QStringLiteral("instruction")).toString(),
             QStringLiteral("沿滨海大道直行约2000米"));
    QCOMPARE(steps.at(1).toMap().value(QStringLiteral("distanceMeters")).toInt(), 800);
}

void MapGeoServiceTest::staticMapWritesPngAndRotatesFile()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    QByteArray png;
    png.append("\x89" "PNG\r\n\x1a\n", 8);
    png.append(QByteArray(64, 'x'));   // 魔数后内容随意：解析只认头部
    server.setResponse(200, png);

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy ready(&service, &MapGeoService::qmlStaticMapReady);
    QSignalSpy failed(&service, &MapGeoService::qmlStaticMapError);
    const QVariantList routePairs = {
        QVariantList{22.541, 113.943}, QVariantList{22.5425, 113.945},
    };
    const QVariantList markerPairs = {
        QVariantMap{{QStringLiteral("latitude"), 22.541},
                    {QStringLiteral("longitude"), 113.943},
                    {QStringLiteral("label"), QStringLiteral("起")}},
        QVariantMap{{QStringLiteral("latitude"), 22.55},
                    {QStringLiteral("longitude"), 113.95}},
    };
    const quint64 id = service.requestStaticMap(22.5455, 113.944, 12, 600, 400,
                                                routePairs, markerPairs);
    QVERIFY2(ready.wait(5000),
             failed.count() ? qPrintable(failed.at(0).at(1).toString()) : "no png response");
    QCOMPARE(ready.at(0).at(0).toULongLong(), id);
    const QString firstFile = ready.at(0).at(1).toString();
    QFile file(firstFile);
    QVERIFY(file.exists());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.read(4), QByteArray("\x89" "PNG", 4));
    file.close();

    // 参数口径（2026-09-08 活体二分后 v2 契约）：path= 单数 kv 才画线（复数 paths=
    // 逗号式被 v2 静默忽略）；markers 用 markers1/markers2 编号、label 仅收单 ASCII
    // 字母数字（中文语义映射 A/B）。线上形态 ':' keep 不编码、'|' 恒 %7C（Qt
    // toPercentEncoding 的 keep 不覆盖 |；服务端百分号解码后等价，活体 PNG 已证）。
    const QString target = server.lastRequestTarget();
    QVERIFY(target.startsWith(QStringLiteral("/ws/staticmap/v2/")));
    QVERIFY(target.contains(QStringLiteral("center=22.545500,113.944000")));
    QVERIFY(target.contains(QStringLiteral("size=600*400")));
    QVERIFY(target.contains(QStringLiteral("path=color:0x00B578%7C22.541000,113.943000%7C22.542500,113.945000")));
    QVERIFY(target.contains(QStringLiteral("markers1=color:0x00A46C%7Clabel:A%7C22.541000,113.943000")));
    QVERIFY(target.contains(QStringLiteral("markers2=color:0x00A46C%7C22.550000,113.950000")));

    // 第二张成功后上一张落盘文件被轮换清理（temp 不累积）。
    service.requestStaticMap(22.5455, 113.944, 12, 600, 400, routePairs, markerPairs);
    QVERIFY(ready.wait(5000) && ready.count() == 2);
    QVERIFY(!QFile::exists(firstFile));
    QVERIFY(QFile::exists(ready.at(1).at(1).toString()));
}

// 活体 414 回归钉（2026-09-08）：京→深驾车折线数千点全量注入 → URL 几十 KB
// → 服务端回 414 URI Too Long（假 HTTP 短折线撞不到）。requestStaticMap 现等距
// 步长抽点 ≤120 + 首尾必含；点数 ≤120 时步长 1、逐字节等价（上一用例的锚即证）。
void MapGeoServiceTest::staticMapDownsamplesLongPolyline()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    QByteArray png;
    png.append("\x89" "PNG\r\n\x1a\n", 8);
    png.append(QByteArray(64, 'x'));
    server.setResponse(200, png);

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy ready(&service, &MapGeoService::qmlStaticMapReady);
    QSignalSpy failed(&service, &MapGeoService::qmlStaticMapError);

    QVariantList routePairs;   // 长路线量级：500 点。append(QVariant(list)) 显式包
                               // 一层——`<<` 会命中 QList 拼接重载，塞成散点。
    for (int index = 0; index < 500; ++index) {
        routePairs.append(QVariant(QVariantList{22.0 + index * 0.0001, 113.0 + index * 0.0001}));
    }
    service.requestStaticMap(22.0, 113.0, 12, 600, 400, routePairs, {});
    QVERIFY2(ready.wait(5000),
             failed.count() ? qPrintable(failed.at(0).at(1).toString()) : "no png response");

    const QString target = server.lastRequestTarget();
    QString pathValue;
    const QStringList segments = target.split(QLatin1Char('&'));
    for (const QString& segment : segments) {
        if (segment.startsWith(QStringLiteral("path="))) {
            pathValue = segment;
        }
    }
    QVERIFY(!pathValue.isEmpty());
    const QStringList drawn = pathValue.split(QStringLiteral("%7C")).mid(1);
    QVERIFY2(!drawn.isEmpty() && drawn.size() <= 120,
             qPrintable(QStringLiteral("drawn=%1").arg(drawn.size())));
    QCOMPARE(drawn.first(), QStringLiteral("22.000000,113.000000"));
    QCOMPARE(drawn.last(), QStringLiteral("22.049900,113.049900"));
    QVERIFY2(target.size() < 4096, qPrintable(QStringLiteral("url=%1").arg(target.size())));
}

void MapGeoServiceTest::staticMapJsonBodyClassifiesError()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    FakeTencentServer server;
    QVERIFY(server.start());
    server.setJsonResponse(QByteArrayLiteral("{\"status\": 310, \"message\": \"key 无效\"}"));

    MapGeoService service;
    service.setEndpointBaseForTesting(server.endpointBase());
    QSignalSpy failed(&service, &MapGeoService::qmlStaticMapError);
    service.requestStaticMap(22.541, 113.943, 12, 600, 400, {}, {});
    QVERIFY(failed.wait(5000));
    // 固定分类文案（不透传 message，防 key 提示进 UI）。
    QCOMPARE(failed.at(0).at(1).toString(),
             QStringLiteral("密钥无效或未授权该接口 [HTTP 200] [status 310]"));
}

void MapGeoServiceTest::navigationUriUrlShapeAndEncoding()
{
    qputenv("TENCENT_MAP_API_KEY", "unit-test-key");
    MapGeoService service;
    const QString url = service.navigationUriUrl(
        22.541, 113.943, QStringLiteral("我的位置"),
        22.55, 113.95, QStringLiteral("深圳湾超充站"));
    QVERIFY(url.startsWith(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan?type=drive")));
    QVERIFY2(url.contains(QStringLiteral("&from=%E6%88%91%E7%9A%84%E4%BD%8D%E7%BD%AE&")),
             qPrintable(url));   // 测试 key=unit-test-key，打印安全
    QVERIFY(url.contains(QStringLiteral("fromcoord=22.541000,113.943000")));
    QVERIFY(url.contains(QStringLiteral("&to=%E6%B7%B1%E5%9C%B3%E6%B9%BE%E8%B6%85%E5%85%85%E7%AB%99&")));
    QVERIFY(url.contains(QStringLiteral("tocoord=22.550000,113.950000")));
    QVERIFY(url.endsWith(QStringLiteral("&referer=unit-test-key")));
}

QTEST_GUILESS_MAIN(MapGeoServiceTest)

#include "tst_map_geo_service.moc"
