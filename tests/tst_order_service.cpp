// OrderService behaviour on the mock transport: list fetch, status filter,
// display-field join, duplicate-submission guard and error propagation.
// Since PR #21 GET_ORDERS is live under contract v1; the mock channel stays
// as the connection-less preview backend, so the raw-request tests below
// double as contract-v1 parity checks (docs/api/user_api_contract.md).

#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/order_service.h"
#include "charging/common/model/enums.h"
#include "charging/common/protocol/protocol.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonValue>
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

// Raw request straight into the mock transport, bypassing the service-layer
// guards — used for contract-v1 parity checks on validation/error semantics.
struct TransportReply
{
    bool done = false;
    bool ok = false;
    QJsonObject data;
    charging::protocol::ProtocolError error;
};

bool sendAndWait(MockRequestTransport& transport, const QString& type, const QJsonObject& data,
                 TransportReply* reply, int timeoutMs = kWaitMs)
{
    transport.send(type, data, [reply](bool ok, const QJsonObject& payload,
                                       const charging::protocol::ProtocolError& error) {
        reply->done = true;
        reply->ok = ok;
        reply->data = payload;
        reply->error = error;
    });
    for (int elapsed = 0; elapsed < timeoutMs && !reply->done; elapsed += 50) {
        QTest::qWait(50);
    }
    return reply->done;
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

    void statusCountsMatchSeededStatuses()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        // Seed: 1 Charging, 1 WaitingPayment, 2 Completed (plus 1 Cancelled).
        QSignalSpy counts(&service, &OrderService::statusCountsUpdated);
        service.fetchStatusCounts();
        QVERIFY(waitForSignal(counts));
        QCOMPARE(counts.count(), 1); // One aggregated emission.
        QCOMPARE(counts.at(0).at(0).toInt(), 1); // charging
        QCOMPARE(counts.at(0).at(1).toInt(), 1); // waiting payment
        QCOMPARE(counts.at(0).at(2).toInt(), 2); // completed
        QVERIFY(!service.isFetchingStatusCounts());
    }

    void duplicateStatusCountFetchIsIgnoredWhileInFlight()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        QSignalSpy counts(&service, &OrderService::statusCountsUpdated);
        service.fetchStatusCounts();
        service.fetchStatusCounts(); // Swallowed by the in-flight guard.
        QVERIFY(waitForSignal(counts));
        QTest::qWait(600);
        QCOMPARE(counts.count(), 1);
    }

    void statusCountFailureDegradesSilentlyAndReleasesGuard()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        // Consume exactly the three badge requests; no emission, no toast.
        transport.setNextFailure(
            QString::fromLatin1(charging::protocol::error_code::kConnectionError), 3);

        QSignalSpy counts(&service, &OrderService::statusCountsUpdated);
        QSignalSpy failed(&service, &OrderService::operationFailed);
        service.fetchStatusCounts();
        QTest::qWait(900); // Past one mock latency window.
        QCOMPARE(counts.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!service.isFetchingStatusCounts());

        // A retry after the transient failure must publish fresh counts.
        service.fetchStatusCounts();
        QVERIFY(waitForSignal(counts));
        QCOMPARE(counts.at(0).at(0).toInt(), 1);
    }

    // ---- contract-v1 parity (docs/api/user_api_contract.md §3) ----

    void ordersComeBackInFrozenSortOrder()
    {
        MockRequestTransport transport;
        OrderService service(&transport);

        QSignalSpy loaded(&service, &OrderService::ordersLoaded);
        service.fetchOrders(OrderService::Filter::All, 1);
        QVERIFY(waitForSignal(loaded));

        // 契约 v1 §3 冻结排序 createdAt DESC、同值 id DESC；页面时间分组
        // （§5）依赖服务端序，客户端不再排。
        const QVector<OrderSummary> orders = loadedOrders(loaded);
        QCOMPARE(orders.size(), 5);
        for (int index = 1; index < orders.size(); ++index) {
            const charging::model::Order& previous = orders.at(index - 1).order;
            const charging::model::Order& current = orders.at(index).order;
            if (previous.createdAtUtc == current.createdAtUtc) {
                QVERIFY2(previous.id > current.id, "tie must break by id DESC");
            } else {
                QVERIFY2(previous.createdAtUtc > current.createdAtUtc,
                         "list must be createdAt DESC");
            }
        }
    }

    void mockOrdersRejectUnknownStatusAndDefaultPagination()
    {
        MockRequestTransport transport;
        const QString ordersType =
            QString::fromLatin1(charging::protocol::request_type::kGetOrders);

        TransportReply badStatus;
        QVERIFY(sendAndWait(transport, ordersType,
                            {{QStringLiteral("status"), QStringLiteral("ALL")}}, &badStatus));
        QVERIFY(!badStatus.ok);
        QCOMPARE(badStatus.error.code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidArgument));
        QCOMPARE(badStatus.error.details.value(QStringLiteral("field")).toString(),
                 QStringLiteral("status")); // 大小写敏感：枚举串按模型 toString。

        TransportReply defaults;
        QVERIFY(sendAndWait(transport, ordersType, QJsonObject{}, &defaults));
        QVERIFY(defaults.ok);
        QCOMPARE(defaults.data.value(QStringLiteral("page")).toInt(), 1);
        QCOMPARE(defaults.data.value(QStringLiteral("pageSize")).toInt(), 20);
        QCOMPARE(defaults.data.value(QStringLiteral("total")).toInt(), 5);

        // 每行必带平铺 join 字段 stationName/chargerCode（契约 v1 §3）。
        const QJsonArray rows = defaults.data.value(QStringLiteral("orders")).toArray();
        QCOMPARE(rows.size(), 5);
        for (const QJsonValue& row : rows) {
            const QJsonObject object = row.toObject();
            QVERIFY(object.contains(QStringLiteral("stationName")));
            QVERIFY(object.contains(QStringLiteral("chargerCode")));
            QVERIFY(!object.value(QStringLiteral("stationName")).toString().isEmpty());
        }
    }
};

QTEST_MAIN(TestOrderService)
#include "tst_order_service.moc"
