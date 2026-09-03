// OrderService behaviour on the mock transport: list fetch, status filter,
// display-field join, duplicate-submission guard and error propagation.
// This exercises the service boundary OrderListPage relies on; it
// intentionally depends on MockRequestTransport (delete together when real
// interfaces land).

#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/order_service.h"
#include "charging/common/model/enums.h"
#include "charging/common/protocol/protocol.h"

#include <QCoreApplication>
#include <QObject>
#include <QSignalSpy>
#include <QTest>

Q_DECLARE_METATYPE(charging::protocol::ProtocolError)

namespace {

using charging::client::MockRequestTransport;
using charging::client::OrderService;
using charging::client::OrderSummary;

constexpr int kWaitMs = 3000;

void registerMetaTypes()
{
    qRegisterMetaType<OrderSummary>("charging::client::OrderSummary");
    qRegisterMetaType<QVector<OrderSummary>>("QVector<charging::client::OrderSummary>");
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

QVector<OrderSummary> loadedOrders(const QSignalSpy& spy)
{
    return qvariant_cast<QVector<OrderSummary>>(spy.at(0).at(0));
}

} // namespace

class TestOrderService final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Callbacks posted by QTimer::singleShot need an event loop.
        QVERIFY(QCoreApplication::instance() != nullptr);
        registerMetaTypes();
    }

    void fetchOrdersReturnsSeedList()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        QSignalSpy loaded(&service, &OrderService::ordersLoaded);
        service.fetchOrders(OrderService::Filter::All, 1);
        QVERIFY(waitForSignal(loaded));

        const QVector<OrderSummary> orders = loadedOrders(loaded);
        QCOMPARE(orders.size(), 5);
        QCOMPARE(loaded.at(0).at(1).toInt(), 5);  // total
        QCOMPARE(loaded.at(0).at(2).toBool(), false); // hasMore
        // Newest first: the in-progress order leads.
        QCOMPARE(charging::model::toString(orders.first().order.status),
                 charging::model::toString(charging::model::OrderStatus::Charging));
        QVERIFY(orders.first().order.orderNo.startsWith(QStringLiteral("MOCKORD")));
        QVERIFY(!orders.first().stationName.isEmpty());
        QVERIFY(!orders.first().chargerCode.isEmpty());
    }

    void filterReturnsOnlyMatchingStatus()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        QSignalSpy loaded(&service, &OrderService::ordersLoaded);
        service.fetchOrders(OrderService::Filter::WaitingPayment, 1);
        QVERIFY(waitForSignal(loaded));

        const QVector<OrderSummary> orders = loadedOrders(loaded);
        QCOMPARE(orders.size(), 1);
        QCOMPARE(charging::model::toString(orders.first().order.status),
                 charging::model::toString(charging::model::OrderStatus::WaitingPayment));
        // Integer billing: 18500 Wh * 132 cents/kWh -> 2442 cents.
        QCOMPARE(orders.first().order.amountCents, qint64(2442));
    }

    void displayFieldsJoinMatchesSeededChargers()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        QSignalSpy loaded(&service, &OrderService::ordersLoaded);
        service.fetchOrders(OrderService::Filter::All, 1);
        QVERIFY(waitForSignal(loaded));

        const QVector<OrderSummary> orders = loadedOrders(loaded);
        QCOMPARE(orders.size(), 5);
        for (const OrderSummary& summary : orders) {
            QVERIFY2(!summary.stationName.isEmpty(), "mock must join station names");
            QVERIFY2(!summary.chargerCode.isEmpty(), "mock must join charger codes");
        }
    }

    void duplicateFetchIsIgnoredWhileInFlight()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        QSignalSpy loaded(&service, &OrderService::ordersLoaded);
        service.fetchOrders(OrderService::Filter::All, 1);
        service.fetchOrders(OrderService::Filter::Completed, 1); // Swallowed.
        QVERIFY(waitForSignal(loaded));

        QTest::qWait(600); // Well past one mock latency window.
        QCOMPARE(loaded.count(), 1);
        QVERIFY(!service.isFetchingOrders());
    }

    void transportFailurePropagatesAndReleasesGuard()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        transport.setNextFailure(
            QString::fromLatin1(charging::protocol::error_code::kConnectionError));

        QSignalSpy failed(&service, &OrderService::operationFailed);
        service.fetchOrders(OrderService::Filter::All, 1);
        QVERIFY(waitForSignal(failed));
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kConnectionError));

        QVERIFY(!service.isFetchingOrders());
        QSignalSpy loaded(&service, &OrderService::ordersLoaded);
        service.fetchOrders(OrderService::Filter::All, 1);
        QVERIFY(waitForSignal(loaded));
        QCOMPARE(loadedOrders(loaded).size(), 5);
    }

    void outOfRangePageReturnsEmptyList()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        QSignalSpy loaded(&service, &OrderService::ordersLoaded);
        QSignalSpy failed(&service, &OrderService::operationFailed);
        service.fetchOrders(OrderService::Filter::All, 9);
        QVERIFY(waitForSignal(loaded));
        QCOMPARE(loadedOrders(loaded).size(), 0);
        QCOMPARE(loaded.at(0).at(2).toBool(), false);
        QCOMPARE(failed.count(), 0);
    }
};

QTEST_MAIN(TestOrderService)
#include "tst_order_service.moc"
