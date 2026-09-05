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
#include "services/settings/settings_service.h"
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
using charging::client::services::settings::SettingsService;
using charging::client::services::settings::Vehicle;

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

// 时间段提交参数：默认从“现在”起 45 分钟（规格上限内）。
void submitSlot(ReservationService& service, const charging::model::Charger& charger,
                const charging::model::Station& station, qint64 vehicleId = 1,
                const QString& plate = QStringLiteral("粤B·D00001"), int minutes = 45,
                int distanceMeters = -1)
{
    const QDateTime start = QDateTime::currentDateTimeUtc();
    service.submit(charger, station, start, start.addSecs(minutes * 60), vehicleId, plate,
                   distanceMeters);
}

Vehicle makeVehicle(const QString& plate)
{
    Vehicle vehicle;
    vehicle.plate = plate;
    vehicle.brandModel = QStringLiteral("测试品牌");
    vehicle.batteryKwh = 60;
    vehicle.connectorType = charging::model::ChargerType::Fast;
    return vehicle;
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
    void mockSubmitInvalidSlotFails();
    void mockSubmitRejectsSlotOverLimit();
    // —— 任务 #17 二次迭代：名额制（车辆数 = 名额，每车至多一条）——
    void mockSubmitRequiresVehicleWhenSettingsInjected();
    void mockSubmitPerVehicleUniquenessAndSlotQuota();
    void mockCancelOnlyForActiveReservation();
    void simulatedFailureCoversListSubmitAndCancel();
    void mockRecordsCanBeOverridden();
    void activeCountTracksStoreAndSlotLimit();
    void expireReservationOnlyConvertsStillActive();
    void cancelLateReservationsConvertsAndFlags();
    void recommendSlotAlignsAndCaps();
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
        // 时间段 + 车辆上下文（二次迭代新增展示字段）。
        QVERIFY(record.startAtUtc.isValid());
        QVERIFY(record.startAtUtc < record.reservation.expiresAtUtc);
        QVERIFY(record.vehicleId > 0);
        QVERIFY(!record.vehiclePlate.isEmpty());
        sawFulfilled |= record.reservation.status == charging::model::ReservationStatus::Fulfilled;
        sawCancelled |= record.reservation.status == charging::model::ReservationStatus::Cancelled;
        sawExpired |= record.reservation.status == charging::model::ReservationStatus::Expired;
    }
    QVERIFY(sawFulfilled);
    QVERIFY(sawCancelled);
    QVERIFY(sawExpired);
    // 最新在前：默认数据里 9001（2 分钟前开始）排第一，9004（25 小时前）排最后。
    QCOMPARE(records.first().reservation.id, qint64(9001));
    QCOMPARE(records.last().reservation.id, qint64(9004));
}

void ReservationServiceTest::mockSubmitSuccessAppendsRecord()
{
    ReservationService service;
    service.setUserId(42);
    // 未注入车辆服务：回退单条约束口径（兼容独立测试）。
    service.setMockRecords({});
    QSignalSpy startedSpy(&service, &ReservationService::submitStarted);
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);

    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addSecs(45 * 60);
    service.submit(makeCharger(7001), makeStation(120), start, end, 7, QStringLiteral("粤B·D00007"));
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
    QCOMPARE(record.durationMinutes, 45);
    // 时间段落库：开始=预约时刻，结束=expiresAtUtc（倒计时数据源不变）。
    QCOMPARE(record.reservation.reservedAtUtc, start);
    QCOMPARE(record.reservation.expiresAtUtc, end);
    QCOMPARE(record.vehicleId, qint64(7));
    QCOMPARE(record.vehiclePlate, QStringLiteral("粤B·D00007"));
    // 预估费用 = 电价(分/度) × 分钟 / 60 = 120×45/60 = 90 分。
    QCOMPARE(record.estimatedFeeCents, qint64(90));

    // 新记录进入列表首位，刷新即可见（预约订单页“成功后展示”依赖此语义）。
    QSignalSpy listSpy(&service, &ReservationService::listSucceeded);
    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listSpy.count() == 1, 3000);
    const auto records = listSpy.at(0).at(0).value<ReservationList>();
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().reservation.chargerId, qint64(7001));
}

void ReservationServiceTest::mockSubmitInvalidSlotFails()
{
    // 参数非法边界：结束 ≤ 开始（时长 ≤ 0）直接失败，不产生记录。
    ReservationService service;
    service.setMockRecords({});
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);

    const QDateTime start = QDateTime::currentDateTimeUtc();
    service.submit(makeCharger(7002), makeStation(), start, start, 1, QStringLiteral("粤B·D00001"));
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QCOMPARE(succeededSpy.count(), 0);
    QVERIFY(failedSpy.at(0).at(0).toString().contains(QStringLiteral("时间段无效")));
}

void ReservationServiceTest::mockSubmitRejectsSlotOverLimit()
{
    // 规格约束：单段超过 45 分钟 → Service 兜底拒绝（UI 行内提示的第一道防线）。
    ReservationService service;
    service.setMockRecords({});
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);

    submitSlot(service, makeCharger(7003), makeStation(), 1, QStringLiteral("粤B·D00001"), 46);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QCOMPARE(succeededSpy.count(), 0);
    QVERIFY(failedSpy.at(0).at(0)
                .toString()
                .contains(QStringLiteral("预约时间段不能超过 45 分钟")));
}

void ReservationServiceTest::mockSubmitRequiresVehicleWhenSettingsInjected()
{
    // 无车辆拦截：设置服务注入且车辆数为 0 → 引导去“设置-车辆管理”。
    SettingsService settings;
    ReservationService service;
    service.setSettingsService(&settings);
    service.setMockRecords({});
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);

    submitSlot(service, makeCharger(7010), makeStation());
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QVERIFY(failedSpy.at(0).at(0)
                .toString()
                .contains(QStringLiteral("请先在设置-车辆管理添加车辆")));

    // 添加车辆后名额随车数演进：1 辆车 = 1 个名额，提交成功。
    settings.setMockVehicles({makeVehicle(QStringLiteral("粤B·D1"))});
    QCOMPARE(service.unfinishedSlotLimit(), 1);
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);
    submitSlot(service, makeCharger(7011), makeStation());
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
}

void ReservationServiceTest::mockSubmitPerVehicleUniquenessAndSlotQuota()
{
    // 名额制（替换上一轮“全局仅一条”）：2 辆车 = 2 个名额，每车至多 1 条。
    SettingsService settings;
    const qint64 firstVehicle
        = settings.addVehicle(makeVehicle(QStringLiteral("粤B·D11111")));
    const qint64 secondVehicle
        = settings.addVehicle(makeVehicle(QStringLiteral("粤B·D22222")));

    ReservationService service;
    service.setSettingsService(&settings);
    service.setMockRecords({});
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy failedSpy(&service, &ReservationService::submitFailed);

    // 第一辆车预约成功。
    submitSlot(service, makeCharger(7003), makeStation(), firstVehicle,
               QStringLiteral("粤B·D11111"));
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    QCOMPARE(service.activeCountForVehicle(firstVehicle), 1);

    // 同一辆车再约 → 按车辆唯一性拒绝。
    submitSlot(service, makeCharger(7003), makeStation(), firstVehicle,
               QStringLiteral("粤B·D11111"));
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 3000);
    QVERIFY(failedSpy.at(0).at(0)
                .toString()
                .contains(QStringLiteral("该车辆已有未结束的预约")));
    QCOMPARE(succeededSpy.count(), 1);

    // 第二辆车 → 名额未用满，成功。
    submitSlot(service, makeCharger(7004), makeStation(), secondVehicle,
               QStringLiteral("粤B·D22222"));
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 2, 3000);
    QCOMPARE(service.activeReservationCount(), 2);

    // 名额已满（2/2）：换新车辆提交也被总数上限拒绝。
    submitSlot(service, makeCharger(7005), makeStation(), firstVehicle + 100,
               QStringLiteral("粤B·D33333"));
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 2, 3000);
    QVERIFY(failedSpy.at(1).at(0)
                .toString()
                .contains(QStringLiteral("可预约名额已全部占用")));
    QCOMPARE(succeededSpy.count(), 2);

    // 结束一条（取消）→ 名额释放，可再发起。
    QSignalSpy cancelSucceededSpy(&service, &ReservationService::cancelSucceeded);
    const auto first = succeededSpy.at(0).at(0).value<ReservationRecord>();
    service.cancel(first.reservation.id);
    QTRY_VERIFY_WITH_TIMEOUT(cancelSucceededSpy.count() == 1, 3000);
    submitSlot(service, makeCharger(7005), makeStation(), firstVehicle,
               QStringLiteral("粤B·D11111"));
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 3, 3000);

    // 并发边界：提交瞬间桩被其他用户抢占（失败原因透传给确认页展示）。
    service.setSimulateNextSubmitConflict(true);
    const auto latest = succeededSpy.at(2).at(0).value<ReservationRecord>();
    service.cancel(latest.reservation.id); // 先释放该车，避免命中唯一性约束
    QTRY_VERIFY_WITH_TIMEOUT(cancelSucceededSpy.count() == 2, 3000);
    settings.setMockVehicles({makeVehicle(QStringLiteral("粤B·D11111")),
                              makeVehicle(QStringLiteral("粤B·D22222")),
                              makeVehicle(QStringLiteral("粤B·D44444"))}); // 扩容避免满额
    submitSlot(service, makeCharger(7006), makeStation(), 999, QStringLiteral("粤B·D99999"));
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 3, 3000);
    QVERIFY(failedSpy.at(2).at(0).toString().contains(QStringLiteral("抢占")));
    QCOMPARE(succeededSpy.count(), 3); // 失败不产生新记录
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
    submitSlot(service, makeCharger(7006), makeStation());
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

void ReservationServiceTest::activeCountTracksStoreAndSlotLimit()
{
    // 名额口径：仅“预约中”且倒计时未归零的记录计入已占用名额。
    ReservationService service;
    QCOMPARE(service.unfinishedSlotLimit(), 1); // 未注入车辆服务：回退单条
    QCOMPARE(service.activeReservationCount(), 1); // 默认数据含 9001 预约中

    SettingsService settings;
    service.setSettingsService(&settings);
    QCOMPARE(service.unfinishedSlotLimit(), 0); // 0 辆车 → 0 名额
    settings.setMockVehicles({makeVehicle(QStringLiteral("粤B·D1")),
                              makeVehicle(QStringLiteral("粤B·D2"))});
    QCOMPARE(service.unfinishedSlotLimit(), 2);
    QCOMPARE(service.activeCountForVehicle(1), 1); // 默认 9001 挂在车辆 1
    QCOMPARE(service.activeCountForVehicle(2), 0);

    // 已过期的“预约中”不算占用名额（expiresAtUtc 已过的记录）。
    ReservationRecord stale;
    stale.reservation.id = 9010;
    stale.reservation.status = charging::model::ReservationStatus::Active;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    stale.startAtUtc = now.addSecs(-7200);
    stale.reservation.reservedAtUtc = stale.startAtUtc;
    stale.reservation.expiresAtUtc = now.addSecs(-3600); // 已到期未流转
    service.setMockRecords({stale});
    QCOMPARE(service.activeReservationCount(), 0);

    // 已结束状态（已完成/已取消）不算占用名额。
    stale.reservation.expiresAtUtc = now.addSecs(3600);
    stale.reservation.status = charging::model::ReservationStatus::Cancelled;
    service.setMockRecords({stale});
    QCOMPARE(service.activeReservationCount(), 0);
    stale.reservation.status = charging::model::ReservationStatus::Active;
    service.setMockRecords({stale});
    QCOMPARE(service.activeReservationCount(), 1);
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
    // 全部结束后不再占用名额。
    QCOMPARE(service.activeReservationCount(), 0);
}

void ReservationServiceTest::cancelLateReservationsConvertsAndFlags()
{
    // 迟到自动取消：开始 + 15 分钟宽限已过、时段仍在有效期内 →
    // “已取消·迟到”（lateCancelled），并逐条发 reservationExpired 刷新。
    ReservationService service;
    const QDateTime now = QDateTime::currentDateTimeUtc();

    const auto makeActive = [now](qint64 id, qint64 vehicleId, int startedMinutesAgo,
                               int durationMinutes) {
        ReservationRecord record;
        record.reservation.id = id;
        record.reservation.status = charging::model::ReservationStatus::Active;
        record.startAtUtc = now.addSecs(-startedMinutesAgo * 60);
        record.reservation.reservedAtUtc = record.startAtUtc;
        record.reservation.expiresAtUtc = record.startAtUtc.addSecs(durationMinutes * 60);
        record.vehicleId = vehicleId;
        record.vehiclePlate = QStringLiteral("粤B·D%1").arg(id);
        record.durationMinutes = durationMinutes;
        return record;
    };
    service.setMockRecords({
        makeActive(9101, 1, 20, 45),  // 迟到 20min（>15）且未过截止 → 取消
        makeActive(9102, 2, 10, 45),  // 迟到 10min（宽限内）→ 保留
        makeActive(9103, 3, 60, 45),  // 已过截止时间 → 交给倒计时归零流转，不抢
    });
    QSignalSpy expiredSpy(&service, &ReservationService::reservationExpired);

    QCOMPARE(service.cancelLateReservations(), 1);
    QCOMPARE(expiredSpy.count(), 1);
    QCOMPARE(expiredSpy.at(0).at(0).toLongLong(), qint64(9101));

    QSignalSpy listSpy(&service, &ReservationService::listSucceeded);
    service.fetchList();
    QTRY_VERIFY_WITH_TIMEOUT(listSpy.count() == 1, 3000);
    const auto records = listSpy.at(0).at(0).value<ReservationList>();
    for (const auto& record : records) {
        if (record.reservation.id == 9101) {
            QVERIFY(record.reservation.status == charging::model::ReservationStatus::Cancelled);
            QVERIFY(record.lateCancelled); // “已取消·迟到”文案依据
        } else if (record.reservation.id == 9102) {
            QVERIFY(record.reservation.status == charging::model::ReservationStatus::Active);
            QVERIFY(!record.lateCancelled);
        } else {
            QVERIFY(record.reservation.status == charging::model::ReservationStatus::Active);
        }
    }
    // 幂等：再次扫描无新增取消（9101 已非“预约中”）。
    QCOMPARE(service.cancelLateReservations(), 0);
}

void ReservationServiceTest::recommendSlotAlignsAndCaps()
{
    // 推荐时段数学：行驶时长 = 5 分钟准备 + 每 500 米 1 分钟（向上取整）；
    // 开始 = 现在 + 行驶时长后向上对齐 15 分钟刻度；时长固定 45 分钟。
    // 固定基准时刻选在整分，避免秒进位歧义。
    const QDateTime base = QDateTime(
        QDate(2026, 9, 4), QTime(10, 7, 0), Qt::UTC);

    const auto near = ReservationService::recommendSlot(0, base);
    QCOMPARE(near.travelMinutes, 5); // 出发准备保底
    QCOMPARE(near.startUtc.secsTo(near.endUtc), 45 * 60);
    // 对齐 15 分钟刻度（本地时间口径与 UTC 一致时按分钟整除验证）。
    const int localSecs
        = near.startUtc.toLocalTime().time().hour() * 3600
        + near.startUtc.toLocalTime().time().minute() * 60
        + near.startUtc.toLocalTime().time().second();
    QCOMPARE(localSecs % (15 * 60), 0);
    QVERIFY(near.startUtc >= base.addSecs(near.travelMinutes * 60));

    const auto far = ReservationService::recommendSlot(2400, base); // 2400m → 5+5=10 分钟
    QCOMPARE(far.travelMinutes, 10);
    const auto round = ReservationService::recommendSlot(1000, base); // → 7 分钟
    QCOMPARE(round.travelMinutes, 7);
}

void ReservationServiceTest::submitCarriesChargerSpecAndDistance()
{
    // 预约上下文扩展字段：充电规格文案 + 虚拟导航距离（预约订单页三栏展示）。
    ReservationService service;
    service.setMockRecords({});
    QSignalSpy succeededSpy(&service, &ReservationService::submitSucceeded);

    submitSlot(service, makeCharger(7008), makeStation(120), 1, QStringLiteral("粤B·D00001"), 45,
               1500);
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() == 1, 3000);
    const auto record = succeededSpy.at(0).at(0).value<ReservationRecord>();
    QCOMPARE(record.chargerSpec, QStringLiteral("直流快充 · 160kW"));
    QCOMPARE(record.distanceMeters, 1500);

    // 交流桩文案。
    QSignalSpy cancelSpy(&service, &ReservationService::cancelSucceeded);
    service.cancel(record.reservation.id);
    QTRY_VERIFY_WITH_TIMEOUT(cancelSpy.count() == 1, 3000);
    charging::model::Charger slow;
    slow.id = 7009;
    slow.code = QStringLiteral("CHG-SLOW-1");
    slow.type = charging::model::ChargerType::Slow;
    slow.powerWatts = 7000;
    QSignalSpy submitSpy(&service, &ReservationService::submitSucceeded);
    submitSlot(service, slow, makeStation(100), 1, QStringLiteral("粤B·D00001"), 30);
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

    submitSlot(service, makeCharger(1), makeStation());
    QTRY_VERIFY_WITH_TIMEOUT(succeededSpy.count() + failedSpy.count() >= 1, 8000);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(succeededSpy.count(), 0);
}

void ReservationServiceTest::liveSubmitAndCancelSucceedEndToEnd()
{
    // 真实接口就绪验证：同一 ClientConnection 登录后，提交/取消全部经
    // RESERVE_CHARGER / CANCEL_RESERVATION 完成，信号形状与模拟通道一致
    // ——接口就绪后 UI 零改动。服务端暂按自身保留时长签发（时间段/车辆
    // 字段的协议扩展就绪前以服务端返回为准，客户端仅带入展示上下文）。
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

    submitSlot(service, makeCharger(fixture.chargerId()), makeStation(), 1,
               QStringLiteral("粤B·D00001"), 30);
    QTRY_VERIFY_WITH_TIMEOUT(submitSucceededSpy.count() + submitFailedSpy.count() >= 1, 8000);
    QCOMPARE(submitFailedSpy.count(), 0);
    const auto created = submitSucceededSpy.at(0).at(0).value<ReservationRecord>();
    QCOMPARE(created.reservation.chargerId, fixture.chargerId());
    QVERIFY(created.reservation.id > 0); // 服务端生成的预约 ID
    QVERIFY(created.reservation.status == charging::model::ReservationStatus::Active);
    // 展示上下文由请求侧带入（与模拟数据同构）。
    QCOMPARE(created.stationName, QStringLiteral("测试充电站"));
    QVERIFY(created.vehiclePlate.isEmpty()); // Server v1 does not persist a vehicle binding.
    QVERIFY(created.orderId > 0);
    // 起止时刻以服务端返回收敛（reservedAt → start，差值为时长）。
    QVERIFY(created.startAtUtc.isValid());
    QCOMPARE(created.startAtUtc, created.reservation.reservedAtUtc);
    QVERIFY(created.durationMinutes > 0);

    QSignalSpy cancelSucceededSpy(&service, &ReservationService::cancelSucceeded);
    QSignalSpy cancelFailedSpy(&service, &ReservationService::cancelFailed);
    service.cancel(created.reservation.id);
    QTRY_VERIFY_WITH_TIMEOUT(cancelSucceededSpy.count() + cancelFailedSpy.count() >= 1, 8000);
    QCOMPARE(cancelFailedSpy.count(), 0);
    QCOMPARE(cancelSucceededSpy.at(0).at(0).toLongLong(), created.reservation.id);

    // 取消释放桩位 → 同一会话再次预约成功（真实通道驱动记录/状态演进）。
    service.submit(makeCharger(fixture.chargerId()), makeStation(),
                   created.startAtUtc, created.startAtUtc.addSecs(30 * 60), 1,
                   QStringLiteral("粤B·D00001"));
    QTRY_VERIFY_WITH_TIMEOUT(submitSucceededSpy.count() == 2, 8000);

    // 服务端裁决的“桩被占用”并发边界：该桩已有进行中的预约（未完成订单）
    // 时，再次提交由服务端拒绝，失败原因经 submitFailed 透传给弹窗。
    // （口径差异说明：服务端唯一索引强制“每用户一条有效预约”，名额制
    // 以模拟通道为准，协议扩展就绪后再提服务端变更。）
    QSignalSpy preemptSucceededSpy(&service, &ReservationService::submitSucceeded);
    QSignalSpy preemptFailedSpy(&service, &ReservationService::submitFailed);
    service.submit(makeCharger(fixture.chargerId()), makeStation(),
                   created.startAtUtc, created.startAtUtc.addSecs(30 * 60), 1,
                   QStringLiteral("粤B·D00001"));
    QTRY_VERIFY_WITH_TIMEOUT(preemptSucceededSpy.count() + preemptFailedSpy.count() >= 1, 8000);
    QCOMPARE(preemptSucceededSpy.count(), 0);
    QCOMPARE(preemptFailedSpy.count(), 1);
    QVERIFY(!preemptFailedSpy.at(0).at(0).toString().isEmpty());
}

QTEST_MAIN(ReservationServiceTest)

#include "tst_reservation_service.moc"
