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
};

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
    QCOMPARE(stations.size(), 5);
    // 模拟数据完整性：金额单位为分，价格与桩位齐备。
    for (const auto& item : stations) {
        QVERIFY(item.station.priceCentsPerKwh > 0);
        QVERIFY(item.station.totalChargers > 0);
        QVERIFY(item.station.availableChargers >= 0);
    }
}

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

QTEST_MAIN(StationQueryServiceTest)

#include "tst_station_query_service.moc"
