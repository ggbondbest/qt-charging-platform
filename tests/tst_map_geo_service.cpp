// 腾讯地图 WebService 封装（MapGeoService）单测：全程走进程内假 HTTP 服务
// （fake_tencent_server.h），永不触真实网络；CI 无 key 也全绿。
#include "fake_tencent_server.h"
#include "services/map/map_geo_service.h"

#include <QCryptographicHash>
#include <QSignalSpy>
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

const QByteArray kRouteJson = R"({
    "status": 0,
    "message": "query ok",
    "result": {"mode": {
        "distance": 5120,
        "duration": 12,
        "steps": [
            {"instruction": "沿滨海大道直行约2000米", "distance": 2000, "duration": 480},
            {"instruction": "在路口右转进入科苑北路", "distance": 800, "duration": 180},
            {"instruction": "到达目的地附近", "distance": 10, "duration": 5}
        ]}}
})";

} // namespace

class MapGeoServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        // 默认无 key 无 SK（用例内按需覆盖，cleanup 统一清除，防泄漏）。
        qputenv("CHARGING_TENCENT_MAP_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_SECRET", "");
    }
    void cleanup()
    {
        qputenv("CHARGING_TENCENT_MAP_KEY", "");
        qputenv("CHARGING_TENCENT_MAP_SECRET", "");
    }

    void noKeyFailsAsyncWithoutAnyNetwork();
    void matrixParsesMultipleDestinations();
    void requestQueryMatchesTencentContract();
    void businessStatusMapsToTypedErrors();
    void transportFailuresMapToTypedErrors();
    void routeParsesDistanceDurationAndSteps();
    void requestIdsAreDistinctAndAscending();
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
        "/distance/v1/matrix/?from=22.541000,113.943000&key=unit-test-key"
        "&mode=driving&to=22.550000,113.950000unit-sk");
    const QByteArray expectedSig =
        QCryptographicHash::hash(expectedRaw, QCryptographicHash::Md5).toHex();
    QVERIFY2(target.contains(QStringLiteral("sig=") + QString::fromLatin1(expectedSig)),
             qPrintable(target));
    // key 绝不出现在信号错误文案里（本用例成功路径无文案，此处保护口径注释）。
}

void MapGeoServiceTest::businessStatusMapsToTypedErrors()
{
    struct { int status; MapError expected; } cases[] = {
        {121, MapError::RateLimited}, // 每日配额超限
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
    // HTTP 403 → RateLimited（签名校验拒绝/配额）
    {
        FakeTencentServer server;
        QVERIFY(server.start());
        server.setResponse(403, QByteArray("{}"));
        MapGeoService service;
        service.setEndpointBaseForTesting(server.endpointBase());
        QSignalSpy failed(&service, &MapGeoService::distanceMatrixFailed);
        service.requestDistanceMatrix({{22.55, 113.95}});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(1).value<MapError>(), MapError::RateLimited);
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

    QVERIFY(server.lastRequestTarget().startsWith(QStringLiteral("/ws/direction/v1/driving/")));
    QVERIFY(server.lastRequestTarget().contains(QStringLiteral("to=22.550000,113.950000")));
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

QTEST_GUILESS_MAIN(MapGeoServiceTest)

#include "tst_map_geo_service.moc"
