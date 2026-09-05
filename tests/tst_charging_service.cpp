// ChargingService behaviour on the mock transport: live status fetch, stop
// transition, payment (success + insufficient balance) and the async guards.
// Exercises the service boundary the charging/settlement pages rely on; it
// depends on MockRequestTransport (delete together when real interfaces land).

#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/common/model/enums.h"
#include "charging/common/protocol/protocol.h"

#include <QCoreApplication>
#include <QObject>
#include <QSignalSpy>
#include <QTest>

Q_DECLARE_METATYPE(charging::protocol::ProtocolError)

namespace {

using charging::client::ChargingService;
using charging::client::ChargingStatus;
using charging::client::MockRequestTransport;

constexpr int kWaitMs = 3000;

void registerMetaTypes()
{
    qRegisterMetaType<ChargingStatus>("charging::client::ChargingStatus");
    qRegisterMetaType<charging::protocol::ProtocolError>("charging::protocol::ProtocolError");
}

bool waitForSignal(QSignalSpy& spy, int timeoutMs = kWaitMs)
{
    // Qt 6.2's QSignalSpy::wait() does not cooperate with custom metatype
    // arguments, so tests poll the spy instead.
    for (int elapsed = 0; elapsed < timeoutMs && spy.isEmpty(); elapsed += 50) {
        QTest::qWait(50);
    }
    return !spy.isEmpty();
}

// The mock seeds exactly one CHARGING order (chargerId 21, "MOCKORD...0005").
constexpr qint64 kChargingOrderId = 5;
// The mock seeds exactly one WAITING_PAYMENT order (chargerId 11, "MOCKORD...0004").
constexpr qint64 kWaitingOrderId = 4;

void stopSeededChargingOrder(ChargingService& service)
{
    // 种子数据里有一笔充电中订单；启动新充电前必须先停掉它，
    // 否则 mock 以 kInvalidStateTransition 拒绝（与服务端单会话约束一致）。
    QSignalSpy stopped(&service, &ChargingService::stopCompleted);
    service.startTracking(kChargingOrderId);
    service.stopCharging();
    QVERIFY(waitForSignal(stopped));
}

} // namespace

class TestChargingService final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(QCoreApplication::instance() != nullptr);
        registerMetaTypes();
    }

    void statusFetchReturnsLiveSnapshot()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        QSignalSpy loaded(&service, &ChargingService::statusLoaded);
        service.startTracking(kChargingOrderId);
        QVERIFY(waitForSignal(loaded));

        const ChargingStatus status = qvariant_cast<ChargingStatus>(loaded.at(0).at(0));
        QCOMPARE(charging::model::toString(status.order.status),
                 charging::model::toString(charging::model::OrderStatus::Charging));
        QVERIFY(status.powerKnown);
        QVERIFY(status.powerWatts > 0);
        // §8.4: the live charge for a CHARGING order rides inside the order.
        QVERIFY(status.order.amountCents > 0);
        QVERIFY(!status.stationName.isEmpty());
        service.stopTracking();
    }

    void statusOfNonChargingOrderSucceedsWithZeroPower()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        // §8.4: querying a non-charging order is a success carrying the
        // persisted order with currentPowerWatts = 0 (not an error).
        QSignalSpy loaded(&service, &ChargingService::statusLoaded);
        QSignalSpy failed(&service, &ChargingService::operationFailed);
        service.startTracking(kWaitingOrderId);
        QVERIFY(waitForSignal(loaded));
        const ChargingStatus status = qvariant_cast<ChargingStatus>(loaded.at(0).at(0));
        QCOMPARE(charging::model::toString(status.order.status),
                 charging::model::toString(charging::model::OrderStatus::WaitingPayment));
        QVERIFY(status.powerKnown);
        QCOMPARE(status.powerWatts, qint64(0));
        QCOMPARE(failed.count(), 0);
        service.stopTracking();
    }

    void stopChargingTransitionsToWaitingPayment()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        QSignalSpy completed(&service, &ChargingService::stopCompleted);
        service.startTracking(kChargingOrderId);
        service.stopCharging();
        QVERIFY(waitForSignal(completed));

        const ChargingStatus status = qvariant_cast<ChargingStatus>(completed.at(0).at(0));
        QCOMPARE(charging::model::toString(status.order.status),
                 charging::model::toString(charging::model::OrderStatus::WaitingPayment));
        QVERIFY(status.order.stoppedAtUtc.isValid());
        // Stopping releases tracking so no further polls fire.
        QVERIFY(!service.isTracking());
    }

    void doubleStopIsIgnoredWhileInFlight()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        QSignalSpy completed(&service, &ChargingService::stopCompleted);
        service.startTracking(kChargingOrderId);
        service.stopCharging();
        service.stopCharging(); // Swallowed by the in-flight guard.
        QVERIFY(waitForSignal(completed));
        QTest::qWait(600);
        QCOMPARE(completed.count(), 1);
    }

    void payOrderDeductsBalance()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        // Seed balance is 100.00 yuan; the waiting order is 24.42 yuan.
        QSignalSpy paid(&service, &ChargingService::paymentCompleted);
        service.payOrder(kWaitingOrderId);
        QVERIFY(waitForSignal(paid));
        QCOMPARE(paid.at(0).at(0).toLongLong(), qint64(2442));
        QCOMPARE(paid.at(0).at(1).toLongLong(), qint64(10000 - 2442));
    }

    void payOrderInsufficientBalancePropagatesError()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        transport.drainBalanceTo(100); // less than the 24.42 yuan order

        QSignalSpy failed(&service, &ChargingService::operationFailed);
        QSignalSpy paid(&service, &ChargingService::paymentCompleted);
        service.payOrder(kWaitingOrderId);
        QVERIFY(waitForSignal(failed));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(paid.count(), 0);
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kInsufficientBalance));
        // Guard must release so a top-up then retry can proceed.
        QVERIFY(!service.isPaying());
    }

    void payCompletedOrderIsIdempotentSuccess()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        QSignalSpy paid(&service, &ChargingService::paymentCompleted);
        service.payOrder(kWaitingOrderId);
        QVERIFY(waitForSignal(paid));
        const qint64 firstAmount = paid.at(0).at(0).toLongLong();
        const qint64 firstBalance = paid.at(0).at(1).toLongLong();

        // §8.6: retrying payment on an already-COMPLETED order is an
        // idempotent success that returns the current result and deducts
        // nothing further (mirrors the leader's no-double-debit rule).
        service.payOrder(kWaitingOrderId);
        for (int elapsed = 0; elapsed < kWaitMs && paid.count() < 2; elapsed += 50) {
            QTest::qWait(50);
        }
        QVERIFY(paid.count() >= 2);
        QCOMPARE(paid.at(1).at(0).toLongLong(), firstAmount);
        QCOMPARE(paid.at(1).at(1).toLongLong(), firstBalance); // balance unchanged
    }

    void stopRetryAfterSettleIsIdempotentSuccess()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        QSignalSpy completed(&service, &ChargingService::stopCompleted);
        service.startTracking(kChargingOrderId);
        service.stopCharging();
        QVERIFY(waitForSignal(completed));
        const ChargingStatus first = qvariant_cast<ChargingStatus>(completed.at(0).at(0));
        QVERIFY(first.order.stoppedAtUtc.isValid());

        // A retry after WAITING_PAYMENT re-settles nothing: same persisted
        // amount, still a success (§8.5).
        service.stopCharging();
        for (int elapsed = 0; elapsed < kWaitMs && completed.count() < 2; elapsed += 50) {
            QTest::qWait(50);
        }
        QVERIFY(completed.count() >= 2);
        const ChargingStatus replay = qvariant_cast<ChargingStatus>(completed.at(1).at(0));
        QCOMPARE(replay.order.amountCents, first.order.amountCents);
        service.stopTracking();
    }

    void unknownOrderRejectedAsNotFound()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);

        QSignalSpy failed(&service, &ChargingService::operationFailed);
        service.payOrder(9999);
        QVERIFY(waitForSignal(failed));
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kNotFound));
    }

    // ---------- START_CHARGING（「充电」Tab 的启动入口） ----------

    void startChargingWalkUpCreatesChargingOrder()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);
        stopSeededChargingOrder(service);

        QSignalSpy started(&service, &ChargingService::startCompleted);
        service.startCharging(0); // reservationId 0 = 模拟扫码（TODO(contract)）
        QVERIFY(waitForSignal(started));

        const ChargingStatus status = qvariant_cast<ChargingStatus>(started.at(0).at(0));
        QCOMPARE(charging::model::toString(status.order.status),
                 charging::model::toString(charging::model::OrderStatus::Charging));
        QCOMPARE(status.stationName, QStringLiteral("云杉科技园区充电站"));
        QCOMPARE(status.chargerCode, QStringLiteral("A05"));
        // 启动成功后服务立即跟踪新订单（充电状态首页据此渲染实时卡）。
        QVERIFY(service.isTracking());
        QVERIFY(!service.isStarting());
        service.stopTracking();
    }

    void startChargingWithRegisteredReservationUsesReservedCharger()
    {
        MockRequestTransport transport;
        transport.registerMockReservation(777, 21, QStringLiteral("万达广场地库充电站"),
                                          QStringLiteral("C02"));
        ChargingService service(&transport);
        stopSeededChargingOrder(service);

        QSignalSpy started(&service, &ChargingService::startCompleted);
        service.startCharging(777);
        QVERIFY(waitForSignal(started));

        const ChargingStatus status = qvariant_cast<ChargingStatus>(started.at(0).at(0));
        QCOMPARE(status.order.reservationId, qint64(777));
        QCOMPARE(status.stationName, QStringLiteral("万达广场地库充电站"));
        QCOMPARE(status.chargerCode, QStringLiteral("C02"));
        service.stopTracking();
    }

    void startChargingUnknownReservationFailsNotFound()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);
        stopSeededChargingOrder(service);

        QSignalSpy failed(&service, &ChargingService::operationFailed);
        QSignalSpy started(&service, &ChargingService::startCompleted);
        service.startCharging(4242);
        QVERIFY(waitForSignal(failed));
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kNotFound));
        QCOMPARE(started.count(), 0);
        QVERIFY(!service.isStarting()); // 失败后守卫释放，可重试
    }

    void startChargingRejectedWhileAnotherOrderIsCharging()
    {
        MockRequestTransport transport; // 种子订单 5 仍在充电中
        ChargingService service(&transport);

        QSignalSpy failed(&service, &ChargingService::operationFailed);
        service.startCharging(0);
        QVERIFY(waitForSignal(failed));
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidStateTransition));
    }

    void doubleStartIsIgnoredWhileInFlight()
    {
        MockRequestTransport transport;
        ChargingService service(&transport);
        stopSeededChargingOrder(service);

        QSignalSpy started(&service, &ChargingService::startCompleted);
        service.startCharging(0);
        QVERIFY(service.isStarting());
        service.startCharging(0); // 在途守卫吞掉第二次
        QVERIFY(waitForSignal(started));
        QTest::qWait(600);
        QCOMPARE(started.count(), 1);
        service.stopTracking();
    }
};

QTEST_MAIN(TestChargingService)
#include "tst_charging_service.moc"
