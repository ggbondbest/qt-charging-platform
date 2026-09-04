#include "billing_service.h"
#include "charging/common/protocol/protocol.h"
#include "charging_repository.h"
#include "charging_server.h"
#include "charging_service.h"
#include "database_connection.h"
#include "network/client_connection.h"
#include "order_repository.h"
#include "order_service.h"
#include "request_dispatcher.h"
#include "services/reservation/reservation_service.h"
#include "tcp_server.h"
#include "user_repository.h"
#include "user_service.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

using namespace charging::client::services::reservation;

charging::model::Charger makeCharger(qint64 id, const QString& code = QStringLiteral("CHG-TEST-1"))
{
    charging::model::Charger charger;
    charger.id = id;
    charger.stationId = 7;
    charger.code = code;
    charger.type = charging::model::ChargerType::Fast;
    charger.powerWatts = 160000;
    charger.status = charging::model::ChargerStatus::Available;
    return charger;
}

charging::model::Station makeStation(qint64 priceCentsPerKwh = 120)
{
    charging::model::Station station;
    station.id = 7;
    station.name = QStringLiteral("测试充电站");
    station.code = QStringLiteral("STA-TEST-7");
    station.priceCentsPerKwh = priceCentsPerKwh;
    return station;
}

// 轻量接缝 fixture：仅登录/用户服务，用于验证未登录与未实现命令的失败路径。
class LightServerFixture final
{
public:
    bool start(QString* errorMessage)
    {
        if (!temporaryDirectory_.isValid()) {
            *errorMessage = QStringLiteral("Unable to create the temporary directory");
            return false;
        }
        const QString databasePath =
            temporaryDirectory_.filePath(QStringLiteral("reservation-light.sqlite3"));
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

// 完整 fixture：与真实部署一致地装配预约命令链（登录 → 预约 → 取消）。
class ReservationServerFixture final
{
public:
    bool start(QString* errorMessage)
    {
        if (!temporaryDirectory_.isValid()) {
            *errorMessage = QStringLiteral("Unable to create the temporary directory");
            return false;
        }
        if (!database_.open(temporaryDirectory_.filePath(
                                QStringLiteral("reservation-live.sqlite3")),
                            false, errorMessage)) {
            return false;
        }

        QSqlQuery query(database_.database());
        if (!query.exec(QStringLiteral("INSERT INTO users (phone, nickname, balance_cents, status)"
                                       " VALUES ('13800000201', '预约测试用户', 10000, 'ACTIVE')"))) {
            *errorMessage = query.lastError().text();
            return false;
        }
        if (!query.exec(QStringLiteral(
                "INSERT INTO stations "
                "(code, name, address, latitude, longitude, price_cents_per_kwh, status) "
                "VALUES ('STA-RES-1', '预约联调站', '测试地址', 38.9, 121.5, 120, 'ACTIVE')"))) {
            *errorMessage = query.lastError().text();
            return false;
        }
        const qint64 stationId = query.lastInsertId().toLongLong();
        query.prepare(QStringLiteral(
            "INSERT INTO chargers "
            "(station_id, code, type, power_watts, status, total_charge_count, "
            " total_charge_seconds) "
            "VALUES (:stationId, 'CHG-RES-1', 'FAST', 160000, 'AVAILABLE', 0, 0)"));
        query.bindValue(QStringLiteral(":stationId"), stationId);
        if (!query.exec()) {
            *errorMessage = query.lastError().text();
            return false;
        }
        chargerId_ = query.lastInsertId().toLongLong();

        userRepository_ = std::make_unique<charging::server::UserRepository>(database_.database());
        chargingRepository_ =
            std::make_unique<charging::server::ChargingRepository>(database_.database());
        orderRepository_ =
            std::make_unique<charging::server::OrderRepository>(database_.database());
        userService_ = std::make_unique<charging::server::UserService>(userRepository_.get());
        billingService_ = std::make_unique<charging::server::BillingService>();
        const charging::server::UtcClock clock = []() { return QDateTime::currentDateTimeUtc(); };
        chargingService_ = std::make_unique<charging::server::ChargingService>(
            chargingRepository_.get(), billingService_.get(), clock);
        orderService_ =
            std::make_unique<charging::server::OrderService>(orderRepository_.get(), clock);
        dispatcher_ = std::make_unique<charging::server::RequestDispatcher>(
            userService_.get(), chargingService_.get(), orderService_.get());
        server_ = std::make_unique<charging::server::TcpServer>();
        server_->setRequestDispatcher(dispatcher_.get());
        return server_->listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const
    {
        return server_->serverPort();
    }

    qint64 chargerId() const
    {
        return chargerId_;
    }

    // 在同一连接上登录（服务端按连接维持会话），供 live 预约鉴权。
    bool login(charging::client::network::ClientConnection& connection)
    {
        QSignalSpy responses(&connection,
                             &charging::client::network::ClientConnection::responseReceived);
        const QString requestId = connection.sendRequest(
            QString::fromLatin1(charging::protocol::request_type::kUserLogin),
            QJsonObject{{QStringLiteral("phone"), QStringLiteral("13800000201")}});
        for (int i = 0; i < 160 && responses.isEmpty(); ++i) {
            QTest::qWait(50);
        }
        for (const auto& arguments : responses) {
            const auto response = arguments.at(0).value<charging::protocol::ResponseEnvelope>();
            if (response.requestId == requestId) {
                return response.success;
            }
        }
        return false;
    }

private:
    QTemporaryDir temporaryDirectory_;
    charging::server::DatabaseConnection database_;
    qint64 chargerId_ = 0;
    std::unique_ptr<charging::server::UserRepository> userRepository_;
    std::unique_ptr<charging::server::ChargingRepository> chargingRepository_;
    std::unique_ptr<charging::server::OrderRepository> orderRepository_;
    std::unique_ptr<charging::server::UserService> userService_;
    std::unique_ptr<charging::server::BillingService> billingService_;
    std::unique_ptr<charging::server::ChargingService> chargingService_;
    std::unique_ptr<charging::server::OrderService> orderService_;
    std::unique_ptr<charging::server::RequestDispatcher> dispatcher_;
    std::unique_ptr<charging::server::TcpServer> server_;
};

} // namespace

class ReservationServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void mockListCoversAllReservationStatusesNewestFirst();
    void mockSubmitSuccessAppendsRecord();
    void mockSubmitInvalidDurationFails();
    void mockSubmitSingleUnfinishedRuleAndPreemption();
    void mockCancelOnlyForActiveReservation();
    void simulatedFailureCoversListSubmitAndCancel();
    void mockRecordsCanBeOverridden();
    // —— 任务 #17 迭代：业务约束与倒计时到期流转 ——
    void hasUnfinishedReservationTracksStore();
    void expireReservationOnlyConvertsStillActive();
    void submitCarriesChargerSpecAndDistance();
    // —— 真实通道接缝 ——
    void liveListWithoutProtocolCommandFailsFriendly();
    void liveSubmitWithoutLoginIsRejected();
    void liveSubmitAndCancelSucceedEndToEnd();
};

void ReservationServiceTest::mockListCoversAllReservationStatusesNewestFirst()
{
    ReservationService service;
    QSignalSpy startedSpy(&service, &ReservationService::listStarted);
    QSignalSpy succeededSpy(&service, &ReservationService::listSucceeded);

    service.fetchList();
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(succeededSpy.count(), 0); // 模拟延迟驱动加载态
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);

    const auto records = succeededSpy.at(0).at(0).value<ReservationList>();
    QCOMPARE(records.size(), 4);
    // 四种状态全覆盖：预约中 / 已完成 / 已取消 / 已过期。
    QVERIFY(records[0].reservation.status == charging::model::ReservationStatus::Active);
    bool sawFulfilled = false;
    bool sawCancelled = false;
    bool sawExpired = false;
    for (const auto& record : records) {
        QVERIFY(!record.stationName.isEmpty());
        QVERIFY(!record.chargerCode.isEmpty());
        QVERIFY(record.durationMinutes > 0);
        QVERIFY(record.estimatedFeeCents > 0);
        sawFulfilled |= record.reservation.status == charging::model::ReservationStatus::Fulfilled;
        sawCancelled |= record.reservation.status == charging::model::ReservationStatus::Cancelled;
        sawExpired |= record.reservation.status == charging::model::ReservationStatus::Expired;
    }
    QVERIFY(sawFulfilled);
    QVERIFY(sawCancelled);
    QVERIFY(sawExpired);
    // 最新在前：默认数据里 9001（10 分钟前）排第一，9004（25 小时前）排最后。
    QCOMPARE(records.first().reservation.id, qint64(9001));
    QCOMPARE(records.last().reservation.id, qint64(9004));
}

void ReservationServiceTest::mockSubmitSuccessAppendsRecord()
{
    ReservationService service;
    service.setUserId(42);
    // 单预约约束：默认模拟数据含“预约中”记录，先清空再演示成功路径。
    service.setMockRecords({});
    QSignalSpy startedSpy(&service, &ReservationService::submitStarted);
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);

    service.submit(makeCharger(7001), makeStation(120), 60);
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).toLongLong(), qint64(7001));
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    QCOMPARE(failedSpy.count(), 0);

    const auto record = succeededSpy.at(0).at(0).value<ReservationRecord>();
    QCOMPARE(record.reservation.chargerId, qint64(7001));
    QCOMPARE(record.reservation.userId, qint64(42));
    QVERIFY(record.reservation.status == charging::model::ReservationStatus::Active);
    QVERIFY(record.reservation.id > 0);
    QCOMPARE(record.stationName, QStringLiteral("测试充电站"));
    QCOMPARE(record.chargerCode, QStringLiteral("CHG-TEST-1"));
    QCOMPARE(record.durationMinutes, 60);
    // 预估费用 = 电价(分/度) × 分钟 / 60 = 120 分。
    QCOMPARE(record.estimatedFeeCents, qint64(120));

    // 新记录进入列表首位，刷新即可见（预约订单页“成功后展示”依赖此语义）。
    QSignalSpy listSpy(&service, &ReservationService::listSucceeded);
    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listSpy.count() == 1, 3000);
    const auto records = listSpy.at(0).at(0).value<ReservationList>();
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().reservation.chargerId, qint64(7001));
}

void ReservationServiceTest::mockSubmitInvalidDurationFails()
{
    // 参数非法边界：预约时长 ≤ 0 直接失败，不产生记录。
    ReservationService service;
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);

    service.submit(makeCharger(7002), makeStation(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QCOMPARE(succeededSpy.count(), 0);
    QVERIFY(failedSpy.at(0).at(0).toString().contains(QStringLiteral("时长")));
}

void ReservationServiceTest::mockSubmitSingleUnfinishedRuleAndPreemption()
{
    ReservationService service;
    service.setMockRecords({}); // 清空默认数据，聚焦单预约约束路径
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);

    // 业务约束：已有未结束预约时再次提交（即使空闲桩）被 Service 兜底拒绝。
    service.submit(makeCharger(7003), makeStation(), 60);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    service.submit(makeCharger(7003), makeStation(), 60);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QVERIFY(failedSpy.at(0).at(0)
                .toString()
                .contains(QStringLiteral("您当前尚有未结束的预约，请结束当前预约后再发起新预约")));
    QCOMPARE(succeededSpy.count(), 1); // 拒绝不产生新记录

    // 结束当前预约（取消）后可再次发起。
    QSignalSpy cancelSucceededSpy(&service, &ReservationService::cancelSucceeded);
    const auto first = succeededSpy.at(0).at(0).value<ReservationRecord>();
    service.cancel(first.reservation.id);
    QTRY_VERIFY_WITH_TIMEOUT(cancelSucceededSpy.count() == 1, 3000);
    service.submit(makeCharger(7003), makeStation(), 60);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 2, 3000);

    // 并发边界：提交瞬间桩被其他用户抢占（失败原因透传给确认页展示）。
    service.setSimulateNextSubmitConflict(true);
    const auto second = succeededSpy.at(1).at(0).value<ReservationRecord>();
    service.cancel(second.reservation.id); // 先结束，避免命中唯一性约束
    QTRY_VERIFY_WITH_TIMEOUT(cancelSucceededSpy.count() == 2, 3000);
    service.submit(makeCharger(7004), makeStation(), 60);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 2, 3000);
    QVERIFY(failedSpy.at(1).at(0).toString().contains(QStringLiteral("抢占")));
    QCOMPARE(succeededSpy.count(), 2); // 失败不产生新记录

    // 抢占开关一次性消耗：下一次提交恢复正常。
    service.submit(makeCharger(7005), makeStation(), 60);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 3, 3000);
}

void ReservationServiceTest::mockCancelOnlyForActiveReservation()
{
    ReservationService service;
    QSignalSpy succeededSpy(&service, &ReservationService::cancelSucceeded);
    QSignalSpy failedSpy(&service, &ReservationService::cancelFailed);

    // 默认数据：9001 预约中可取消；9002 已完成、9004 已过期均不可。
    service.cancel(9001);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    QCOMPARE(succeededSpy.at(0).at(0).toLongLong(), qint64(9001));

    service.cancel(9002);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QVERIFY(failedSpy.at(0).at(0).toString().contains(QStringLiteral("已结束")));

    // 记录不存在：友好提示“刷新后重试”。
    service.cancel(12345);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 2, 3000);
    QVERIFY(failedSpy.at(1).at(0).toString().contains(QStringLiteral("不存在")));

    // 取消成功后列表体现“已取消”状态（页面刷新依赖此语义）。
    QSignalSpy listSpy(&service, &ReservationService::listSucceeded);
    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listSpy.count() == 1, 3000);
    const auto records = listSpy.at(0).at(0).value<ReservationList>();
    QCOMPARE(records.size(), 4);
    for (const auto& record : records) {
        if (record.reservation.id == 9001) {
            QVERIFY(record.reservation.status == charging::model::ReservationStatus::Cancelled);
        }
    }
}

void ReservationServiceTest::simulatedFailureCoversListSubmitAndCancel()
{
    ReservationService service;
    QSignalSpy listFailedSpy(&service, &ReservationService::listFailed);
    QSignalSpy submitFailedSpy(&service, &ReservationService::submitFailed);
    QSignalSpy cancelFailedSpy(&service, &ReservationService::cancelFailed);

    service.setSimulateFailure(true);
    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listFailedSpy.count() == 1, 3000);

    service.setSimulateFailure(true);
    service.submit(makeCharger(7006), makeStation(), 60);
    QTRY_VERIFY_WITH_TIMEOUT(submitFailedSpy.count() == 1, 3000);

    service.setSimulateFailure(true);
    service.cancel(9001);
    QTRY_VERIFY_WITH_TIMEOUT(cancelFailedSpy.count() == 1, 3000);

    // 失败开关一次性消耗：恢复后正常返回（页面错误态“重试”可恢复）。
    QSignalSpy listSucceededSpy(&service, &ReservationService::listSucceeded);
    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listSucceededSpy.count() == 1, 3000);
}

void ReservationServiceTest::mockRecordsCanBeOverridden()
{
    // 空覆盖驱动预约模块“暂无记录”空态（订单页 / 已完成页共用列表通道）。
    ReservationService service;
    service.setMockRecords({});
    QSignalSpy listSpy(&service, &ReservationService::listSucceeded);

    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listSpy.count() == 1, 3000);
    QCOMPARE(listSpy.at(0).at(0).value<ReservationList>().size(), 0);
}

void ReservationServiceTest::hasUnfinishedReservationTracksStore()
{
    // 业务约束查询：仅“预约中”且倒计时未归零的记录计为未结束。
    ReservationService service;
    QVERIFY(service.hasUnfinishedReservation()); // 默认数据含 9001 预约中

    // 已过期的“预约中”不算未结束（expiresAtUtc 已过的记录）。
    ReservationRecord stale;
    stale.reservation.id = 9010;
    stale.reservation.status = charging::model::ReservationStatus::Active;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    stale.reservation.reservedAtUtc = now.addSecs(-7200);
    stale.reservation.expiresAtUtc = now.addSecs(-3600); // 已到期未流转
    service.setMockRecords({stale});
    QVERIFY(!service.hasUnfinishedReservation());

    // 已结束状态（已完成/已取消）不算未结束。
    stale.reservation.expiresAtUtc = now.addSecs(3600);
    stale.reservation.status = charging::model::ReservationStatus::Cancelled;
    service.setMockRecords({stale});
    QVERIFY(!service.hasUnfinishedReservation());
    stale.reservation.status = charging::model::ReservationStatus::Active;
    service.setMockRecords({stale});
    QVERIFY(service.hasUnfinishedReservation());
}

void ReservationServiceTest::expireReservationOnlyConvertsStillActive()
{
    // 倒计时归零流转：仅“预约中”记录被置为“已过期”；重复调用幂等，
    // 已结束记录不受影响，流转信号始终发出（页面据此刷新）。
    ReservationService service;
    QSignalSpy expiredSpy(&service, &ReservationService::reservationExpired);
    QSignalSpy listSpy(&service, &ReservationService::listSucceeded);

    service.expireReservation(9001); // Active → Expired
    QCOMPARE(expiredSpy.count(), 1);
    service.expireReservation(9001); // 重复流转：状态不变，仍发信号
    QCOMPARE(expiredSpy.count(), 2);
    service.expireReservation(9002); // Fulfilled 不受影响
    service.expireReservation(9999); // 不存在的记录：不崩溃
    QCOMPARE(expiredSpy.count(), 4);

    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listSpy.count() == 1, 3000);
    const auto records = listSpy.at(0).at(0).value<ReservationList>();
    QCOMPARE(records.size(), 4);
    int expiredCount = 0;
    for (const auto& record : records) {
        if (record.reservation.id == 9001 || record.reservation.id == 9004) {
            QVERIFY(record.reservation.status == charging::model::ReservationStatus::Expired);
            ++expiredCount;
        }
        if (record.reservation.id == 9002) {
            QVERIFY(record.reservation.status
                    == charging::model::ReservationStatus::Fulfilled);
        }
    }
    QCOMPARE(expiredCount, 2);
    // 全部结束后不再阻断新预约。
    QVERIFY(!service.hasUnfinishedReservation());
}

void ReservationServiceTest::submitCarriesChargerSpecAndDistance()
{
    // 预约上下文扩展字段：充电规格文案 + 虚拟导航距离（预约订单页三栏展示）。
    ReservationService service;
    service.setMockRecords({});
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);

    service.submit(makeCharger(7008), makeStation(120), 60, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    const auto record = succeededSpy.at(0).at(0).value<ReservationRecord>();
    QCOMPARE(record.chargerSpec, QStringLiteral("直流快充 · 160kW"));
    QCOMPARE(record.distanceMeters, 1500);

    // 交流桩文案。
    service.cancel(record.reservation.id);
    QSignalSpy cancelSpy(&service, &ReservationService::cancelSucceeded);
    QTRY_VERIFY_WITH_TIMEOUT(cancelSpy.count() == 1, 3000);
    charging::model::Charger slow;
    slow.id = 7009;
    slow.code = QStringLiteral("CHG-SLOW-1");
    slow.type = charging::model::ChargerType::Slow;
    slow.powerWatts = 7000;
    QSignalSpy submitSpy(&service, &ReservationService::submitSucceeded);
    service.submit(slow, makeStation(100), 30);
    QTRY_VERIFY_WITH_TIMEOUT(submitSpy.count() == 1, 3000);
    const auto second = submitSpy.at(0).at(0).value<ReservationRecord>();
    QCOMPARE(second.chargerSpec, QStringLiteral("交流慢充 · 7kW"));
    QCOMPARE(second.distanceMeters, -1); // 未提供距离：占位缺省
}

void ReservationServiceTest::liveListWithoutProtocolCommandFailsFriendly()
{
    // 真实通道接缝：协议尚未定义预约列表命令，liveMode 下应得到友好失败
    // （页面错误态），而非误回模拟数据或崩溃。
    LightServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(QStringLiteral("127.0.0.1"),
                                                           fixture.port());
    ReservationService service;
    service.setConnection(&connection);
    service.setLiveMode(true);
    QSignalSpy succeededSpy(&service, &ReservationService::listSucceeded);
    QSignalSpy failedSpy(&service, &ReservationService::listFailed);

    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() + failedSpy.count() >= 1, 8000);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(succeededSpy.count(), 0);
    QVERIFY(!failedSpy.at(0).at(0).toString().isEmpty());
}

void ReservationServiceTest::liveSubmitWithoutLoginIsRejected()
{
    // 鉴权边界：未登录会话提交预约 → 服务端拒绝（弹窗展示失败原因）。
    LightServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(QStringLiteral("127.0.0.1"),
                                                           fixture.port());
    ReservationService service;
    service.setConnection(&connection);
    service.setLiveMode(true);
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);

    service.submit(makeCharger(1), makeStation(), 60);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() + failedSpy.count() >= 1, 8000);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(succeededSpy.count(), 0);
}

void ReservationServiceTest::liveSubmitAndCancelSucceedEndToEnd()
{
    // 真实接口就绪验证：同一 ClientConnection 登录后，提交/取消全部经
    // RESERVE_CHARGER / CANCEL_RESERVATION 完成，信号形状与模拟通道一致
    // ——接口就绪后 UI 零改动。
    ReservationServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::ClientConnection connection(QStringLiteral("127.0.0.1"),
                                                           fixture.port());
    QVERIFY2(fixture.login(connection), "LOGIN request must succeed");

    ReservationService service;
    service.setConnection(&connection);
    service.setLiveMode(true);
    service.setUserId(1);

    QSignalSpy submitSucceededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy submitFailedSpy(&service, &ReservationService::submitFailed);

    service.submit(makeCharger(fixture.chargerId()), makeStation(), 90);
    QTRY_VERIFY_WITH_TIMEOUT(submitSucceededSpy.count() + submitFailedSpy.count() >= 1, 8000);
    QCOMPARE(submitFailedSpy.count(), 0);
    const auto created = submitSucceededSpy.at(0).at(0).value<ReservationRecord>();
    QCOMPARE(created.reservation.chargerId, fixture.chargerId());
    QVERIFY(created.reservation.id > 0); // 服务端生成的预约 ID
    QVERIFY(created.reservation.status == charging::model::ReservationStatus::Active);
    // 展示上下文由请求侧带入（与模拟数据同构）。
    QCOMPARE(created.stationName, QStringLiteral("测试充电站"));
    QCOMPARE(created.durationMinutes, 90);

    QSignalSpy cancelSucceededSpy(&service, &ReservationService::cancelSucceeded);
    QSignalSpy cancelFailedSpy(&service, &ReservationService::cancelFailed);
    service.cancel(created.reservation.id);
    QTRY_VERIFY_WITH_TIMEOUT(cancelSucceededSpy.count() + cancelFailedSpy.count() >= 1, 8000);
    QCOMPARE(cancelFailedSpy.count(), 0);
    QCOMPARE(cancelSucceededSpy.at(0).at(0).toLongLong(), created.reservation.id);

    // 取消释放桩位 → 同一会话再次预约成功（真实通道驱动记录/状态演进）。
    service.submit(makeCharger(fixture.chargerId()), makeStation(), 90);
    QTRY_VERIFY_WITH_TIMEOUT(submitSucceededSpy.count() == 2, 8000);

    // 服务端裁决的“桩被占用”并发边界：该桩已有进行中的预约（未完成订单）
    // 时，再次提交由服务端拒绝，失败原因经 submitFailed 透传给弹窗。
    QSignalSpy preemptSucceededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy preemptFailedSpy(&service, &ReservationService::submitFailed);
    service.submit(makeCharger(fixture.chargerId()), makeStation(), 30);
    QTRY_VERIFY_WITH_TIMEOUT(preemptSucceededSpy.count() + preemptFailedSpy.count() >= 1, 8000);
    QCOMPARE(preemptSucceededSpy.count(), 0);
    QCOMPARE(preemptFailedSpy.count(), 1);
    QVERIFY(!preemptFailedSpy.at(0).at(0).toString().isEmpty());
}

QTEST_MAIN(ReservationServiceTest)

#include "tst_reservation_service.moc"
