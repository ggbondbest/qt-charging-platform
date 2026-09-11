// ---- 文件说明：StationQueryService 站点查询单测（列表搜索 + 详情，各含模拟/live 双接缝）----
// boot：QTEST_MAIN —— 模拟通道用 QTimer 延迟驱动 queryStarted→succeeded/failed
//   信号，需要事件循环。
// 隔离：模拟通道每用例独立栈上 service；live 用例用 ServiceServerFixture ——
//   QTemporaryDir 专属 SQLite + 端口 0 随机监听，析构即清理。
//   该 fixture 只挂 UserService，GET_STATIONS / GET_CHARGERS 天然未实现，
//   专门验证“命令缺位 → 友好失败”的降级路径，而非成功路径。
#include "charging_server.h"
#include "database_connection.h"
#include "network/client_connection.h"
#include "request_dispatcher.h"
#include "services/station/station_query_service.h"
#include "user_repository.h"
#include "user_service.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

using namespace charging::client::services::station;

// ---- 最小真实服务端：仅登录域装配，站点/桩命令缺位（live 降级接缝的靶子）----
class ServiceServerFixture final
{
public:
    bool start(QString* errorMessage)
    {
        if (!temporaryDirectory_.isValid()) {
            *errorMessage = QStringLiteral("Unable to create the temporary directory");
            return false;
        }
        const QString databasePath =
            temporaryDirectory_.filePath(QStringLiteral("station-query-test.sqlite3"));
        if (!database_.open(databasePath, false, errorMessage)) {
            return false;
        }
        repository_ = std::make_unique<charging::server::UserRepository>(database_.database());
        service_ = std::make_unique<charging::server::UserService>(repository_.get());
        dispatcher_ = std::make_unique<charging::server::RequestDispatcher>(service_.get());
        server_ = std::make_unique<charging::server::ChargingServer>();
        server_->setRequestDispatcher(dispatcher_.get());
        return server_->listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const
    {
        return server_->serverPort();
    }

private:
    QTemporaryDir temporaryDirectory_;
    charging::server::DatabaseConnection database_;
    std::unique_ptr<charging::server::UserRepository> repository_;
    std::unique_ptr<charging::server::UserService> service_;
    std::unique_ptr<charging::server::RequestDispatcher> dispatcher_;
    std::unique_ptr<charging::server::ChargingServer> server_;
};

} // namespace

class StationQueryServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void mockSearchEmitsStartedThenAllStations();
    void mockKeywordFiltersNameAndAddress();
    void simulatedFailureEmitsQueryFailed();
    void liveChannelWithoutServerImplementationEmitsFriendlyFailure();
    // —— 任务 #12：站点详情通道 ——
    void mockFetchDetailReturnsChargersCoveringAllStatuses();
    void mockFetchDetailEmptyChargerStation();
    void mockFetchDetailOfflineStationAndUnknownId();
    void simulatedFailureEmitsDetailFailed();
    void liveDetailChannelWithoutServerImplementationEmitsFriendlyFailure();
};

// ---- 测：模拟列表通道信号时序与 6 演示站完整性；钉：金额/桩位计数自洽、离线站与无桩站保留为边界数据 ----
void StationQueryServiceTest::mockSearchEmitsStartedThenAllStations()
{
    StationQueryService service;
    QSignalSpy startedSpy(&service, &StationQueryService::queryStarted);
    QSignalSpy succeededSpy(&service, &StationQueryService::querySucceeded);

    service.search();

    QCOMPARE(startedSpy.count(), 1);
    // 模拟通道带延迟：先只有 started。
    QCOMPARE(succeededSpy.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);

    const auto stations = succeededSpy.at(0).at(0).value<StationList>();
    // 6 条演示站点：含离线站（id4）与无桩站（id6），驱动详情页边界状态。
    QCOMPARE(stations.size(), 6);
    // 模拟数据完整性：金额单位为分，桩位计数自洽。
    for (const auto& item : stations) {
        QVERIFY(item.station.priceCentsPerKwh > 0);
        QVERIFY(item.station.totalChargers >= 0);
        QVERIFY(item.station.availableChargers >= 0);
        QVERIFY(item.station.availableChargers <= item.station.totalChargers);
    }
}

// ---- 测：关键词分别命中站名与地址；钉：过滤不误伤、命中结果仍走 succeeded 信号 ----
void StationQueryServiceTest::mockKeywordFiltersNameAndAddress()
{
    StationQueryService service;
    QSignalSpy succeededSpy(&service, &StationQueryService::querySucceeded);

    service.search(QStringLiteral("科技园"));
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    QCOMPARE(succeededSpy.at(0).at(0).value<StationList>().size(), 1);

    succeededSpy.clear();
    service.search(QStringLiteral("滨海"));
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    const auto results = succeededSpy.at(0).at(0).value<StationList>();
    QCOMPARE(results.size(), 1);
    QVERIFY(results.at(0).station.name.contains(QStringLiteral("滨海")));
}

// ---- 测：列表失败开关驱动 queryFailed；钉：失败消息为用户可读文案（页面直显、无错误码）----
void StationQueryServiceTest::simulatedFailureEmitsQueryFailed()
{
    StationQueryService service;
    service.setSimulateFailure(true);
    QSignalSpy failedSpy(&service, &StationQueryService::queryFailed);
    QSignalSpy succeededSpy(&service, &StationQueryService::querySucceeded);

    service.search(QStringLiteral("充电"));
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QCOMPARE(succeededSpy.count(), 0);
    // 失败消息面向用户可读（页面直接展示，不出现错误码）。
    QVERIFY(!failedSpy.at(0).at(0).toString().isEmpty());
}

// ---- 测：live 下 GET_STATIONS 未实现的降级；钉：走请求-失败路径，不崩溃、不误回模拟数据 ----
void StationQueryServiceTest::liveChannelWithoutServerImplementationEmitsFriendlyFailure()
{
    // 真实通道接缝验证：服务端尚未实现 GET_STATIONS 时，liveMode 应走
    // 请求-失败路径并给出友好错误（而不是崩溃或误回模拟数据）。
    ServiceServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(QStringLiteral("127.0.0.1"),
                                                           fixture.port());
    StationQueryService service;
    service.setConnection(&connection);
    service.setLiveMode(true);

    QSignalSpy failedSpy(&service, &StationQueryService::queryFailed);
    QSignalSpy succeededSpy(&service, &StationQueryService::querySucceeded);

    service.search(QStringLiteral("科技园"));
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() + succeededSpy.count() >= 1, 8000);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(succeededSpy.count(), 0);
}

namespace {

// 详情页只需站点 ID：其余字段由服务端/模拟源回查覆盖。
charging::model::Station stationWithId(qint64 id)
{
    charging::model::Station station;
    station.id = id;
    return station;
}

StationDetail fetchDetailAndWait(StationQueryService& service, qint64 stationId,
                                 QSignalSpy& succeededSpy, QSignalSpy& failedSpy)
{
    service.fetchDetail(stationWithId(stationId), 850);
    // QTRY_* 宏只能用于测试槽函数，这里手工等待并留出断言空间。
    for (int i = 0; i < 60 && succeededSpy.count() + failedSpy.count() == 0; ++i) {
        QTest::qWait(50);
    }
    return succeededSpy.isEmpty() ? StationDetail{}
                                  : succeededSpy.at(0).at(0).value<StationDetail>();
}

} // namespace

// ---- 测：站 1 详情的桩位数据；钉：总数/可用数自洽且空闲/占用/故障/离线四态齐备（详情页图例依赖）----
void StationQueryServiceTest::mockFetchDetailReturnsChargersCoveringAllStatuses()
{
    StationQueryService service;
    QSignalSpy startedSpy(&service, &StationQueryService::detailStarted);
    QSignalSpy succeededSpy(&service, &StationQueryService::detailSucceeded);
    QSignalSpy failedSpy(&service, &StationQueryService::detailFailed);

    const auto detail = fetchDetailAndWait(service, 1, succeededSpy, failedSpy);
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(detail.hasChargerData);
    QCOMPARE(detail.station.id, qint64(1));
    QCOMPARE(detail.distanceMeters, 850);
    // 桩位总数与站点空位数自洽，且覆盖 空闲/占用/故障/离线 四类状态。
    QCOMPARE(detail.chargers.size(), detail.station.totalChargers);
    int available = 0;
    bool sawCharging = false;
    bool sawFault = false;
    bool sawOffline = false;
    for (const auto& charger : detail.chargers) {
        QCOMPARE(charger.stationId, detail.station.id);
        QVERIFY(!charger.code.isEmpty());
        QVERIFY(charger.powerWatts > 0);
        switch (charger.status) {
        case charging::model::ChargerStatus::Available:
            ++available;
            break;
        case charging::model::ChargerStatus::Charging:
        case charging::model::ChargerStatus::Reserved:
            sawCharging = true;
            break;
        case charging::model::ChargerStatus::Fault:
            sawFault = true;
            break;
        case charging::model::ChargerStatus::Offline:
            sawOffline = true;
            break;
        }
    }
    QCOMPARE(available, detail.station.availableChargers);
    QVERIFY(sawCharging);
    QVERIFY(sawFault);
    QVERIFY(sawOffline);
}

// ---- 测：无桩站（id6）详情边界；钉：空桩列表仍算成功回包（页面展示空态而非报错）----
void StationQueryServiceTest::mockFetchDetailEmptyChargerStation()
{
    // 空数据边界：id6 为无桩演示站点。
    StationQueryService service;
    QSignalSpy succeededSpy(&service, &StationQueryService::detailSucceeded);
    QSignalSpy failedSpy(&service, &StationQueryService::detailFailed);

    const auto detail = fetchDetailAndWait(service, 6, succeededSpy, failedSpy);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(detail.hasChargerData);
    QCOMPARE(detail.chargers.size(), 0);
    QCOMPARE(static_cast<int>(detail.station.status),
             static_cast<int>(charging::model::StationStatus::Active));
}

// ---- 测：离线站（id4）/未知 ID/无 ID 三种非法或降级输入；钉：均走 detailFailed 或数据源状态，不崩溃不误回空数据 ----
void StationQueryServiceTest::mockFetchDetailOfflineStationAndUnknownId()
{
    StationQueryService service;
    QSignalSpy succeededSpy(&service, &StationQueryService::detailSucceeded);
    QSignalSpy failedSpy(&service, &StationQueryService::detailFailed);

    // 离线边界：id4 站点状态由数据源给出 Inactive（页面据此展示横幅）。
    const auto detail = fetchDetailAndWait(service, 4, succeededSpy, failedSpy);
    QCOMPARE(static_cast<int>(detail.station.status),
             static_cast<int>(charging::model::StationStatus::Inactive));
    for (const auto& charger : detail.chargers) {
        QCOMPARE(static_cast<int>(charger.status),
                 static_cast<int>(charging::model::ChargerStatus::Offline));
    }

    succeededSpy.clear();
    failedSpy.clear();
    // 未知 ID（路由携带非法站点）：回友好错误而非崩溃或误回空数据。
    fetchDetailAndWait(service, 999, succeededSpy, failedSpy);
    QCOMPARE(succeededSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!failedSpy.at(0).at(0).toString().isEmpty());

    // 无站点 ID：立即走失败路径。
    succeededSpy.clear();
    failedSpy.clear();
    QSignalSpy startedSpy(&service, &StationQueryService::detailStarted);
    service.fetchDetail(stationWithId(0), 0);
    QCOMPARE(startedSpy.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QCOMPARE(succeededSpy.count(), 0);
}

// ---- 测：详情通道的失败开关驱动 detailFailed；钉：与列表开关独立、消息可读 ----
void StationQueryServiceTest::simulatedFailureEmitsDetailFailed()
{
    StationQueryService service;
    service.setSimulateFailure(true);
    QSignalSpy succeededSpy(&service, &StationQueryService::detailSucceeded);
    QSignalSpy failedSpy(&service, &StationQueryService::detailFailed);

    fetchDetailAndWait(service, 1, succeededSpy, failedSpy);
    QCOMPARE(succeededSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!failedSpy.at(0).at(0).toString().isEmpty());
}

// ---- 测：live 下 GET_CHARGERS 未实现的详情降级；钉：友好失败（页面据此展示异常态+返回首页）----
void StationQueryServiceTest::liveDetailChannelWithoutServerImplementationEmitsFriendlyFailure()
{
    // 真实通道接缝验证：服务端未实现 GET_CHARGERS 时，liveMode 详情走
    // 请求-失败路径给出友好错误（页面据此展示异常态 + 返回首页）。
    ServiceServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(QStringLiteral("127.0.0.1"),
                                                           fixture.port());
    StationQueryService service;
    service.setConnection(&connection);
    service.setLiveMode(true);

    QSignalSpy succeededSpy(&service, &StationQueryService::detailSucceeded);
    QSignalSpy failedSpy(&service, &StationQueryService::detailFailed);

    service.fetchDetail(stationWithId(1), 850);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() + failedSpy.count() >= 1, 8000);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(succeededSpy.count(), 0);
}

QTEST_MAIN(StationQueryServiceTest)

#include "tst_station_query_service.moc"
