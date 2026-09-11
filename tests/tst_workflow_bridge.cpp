#include "workflow_bridge.h"
#include "network/client_connection.h"
#include "charging/client/profile_charging/i_request_transport.h"
#include <QSignalSpy>
#include <QJsonArray>
#include <QTimer>
#include <QtTest>
using charging::qml::WorkflowBridge;
using charging::client::network::ClientConnection;

class DeferredTransport final : public charging::client::IRequestTransport
{
public:
    struct Call { QString type; ResponseCallback callback; };
    QList<Call> calls;
    void send(const QString& type, const QJsonObject&, const ResponseCallback& callback) override
    { calls.append({type, callback}); }
    ResponseCallback take(const QString& type)
    {
        for (int i = 0; i < calls.size(); ++i)
            if (calls.at(i).type == type) return calls.takeAt(i).callback;
        return {};
    }
};

class WorkflowBridgeTest final : public QObject
{
    Q_OBJECT
private slots:
    void subscribeRetryAndDroppedEventFallback()
    {
        DeferredTransport transport;
        ClientConnection connection;
        WorkflowBridge bridge(&transport, &connection, true);
        QSignalSpy changed(&bridge, &WorkflowBridge::changed);
        QTRY_COMPARE(transport.calls.size(), 2);
        auto subscribe = transport.take("WORKFLOW_SUBSCRIBE");
        auto read = transport.take("QUEUE_GET_MINE");
        QVERIFY(subscribe && read);
        subscribe(false, {}, {"DATABASE_ERROR", "retry", {}});
        read(true, {{"item", QJsonObject{}}}, {});
        auto* fallback = bridge.findChild<QTimer*>("workflowRefreshTimer");
        QVERIFY(fallback);
        QVERIFY(QMetaObject::invokeMethod(fallback, "timeout", Qt::DirectConnection));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(transport.calls.size(), 2);
        subscribe = transport.take("WORKFLOW_SUBSCRIBE"); read = transport.take("QUEUE_GET_MINE");
        QVERIFY(subscribe && read);
        subscribe(true, {{"subscribed", true}}, {}); read(true, {}, {});
        QVERIFY(QMetaObject::invokeMethod(fallback, "timeout", Qt::DirectConnection));
        QCOMPARE(changed.size(), 2);
        QCOMPARE(transport.calls.size(), 1); // Successful subscription is not repeated.
        QCOMPARE(transport.calls.first().type, QStringLiteral("QUEUE_GET_MINE"));
    }

    void eventsCoalesceAndCallOnlyAnnouncesOnce()
    {
        DeferredTransport transport;
        ClientConnection connection;
        WorkflowBridge bridge(&transport, &connection, true);
        QSignalSpy calls(&bridge, &WorkflowBridge::callAvailable);
        QSignalSpy changed(&bridge, &WorkflowBridge::changed);
        QTRY_COMPARE(transport.calls.size(), 2);
        transport.take("WORKFLOW_SUBSCRIBE")(true, {}, {});
        const QJsonObject called{{"id", "7"}, {"status", "CALLED"}, {"calledAt", "2026-09-09T00:00:00.000Z"}};
        auto read = transport.take("QUEUE_GET_MINE");
        connection.eventReceived("WORKFLOW_CHANGED", {});
        connection.eventReceived("WORKFLOW_CHANGED", {});
        QCOMPARE(changed.size(), 2);
        QVERIFY(transport.calls.isEmpty()); // Existing read remains the sole read.
        read(true, {{"item", called}}, {});
        QCOMPARE(calls.size(), 1);
        connection.eventReceived("WORKFLOW_CHANGED", {});
        QCOMPARE(transport.calls.size(), 1);
        transport.take("QUEUE_GET_MINE")(true, {{"item", called}}, {});
        QCOMPARE(calls.size(), 1);
        connection.eventReceived("UNKNOWN_EVENT", {});
        QCOMPARE(changed.size(), 3);
        QVERIFY(transport.calls.isEmpty());
    }

    void destroyedSessionDropsPendingReplyAndMockIsExplicit()
    {
        DeferredTransport transport;
        ClientConnection connection;
        auto* bridge = new WorkflowBridge(&transport, &connection, true);
        const auto requestId = bridge->request("REPAIR_GET_MINE");
        QVERIFY(!requestId.isEmpty());
        auto callback = transport.take("REPAIR_GET_MINE");
        QVERIFY(callback);
        delete bridge;
        callback(true, {{"items", QJsonArray{}}}, {}); // Guarded callback is now a no-op.
        WorkflowBridge preview(&transport, &connection, false);
        QSignalSpy finished(&preview, &WorkflowBridge::finished);
        const auto id = preview.request("QUEUE_JOIN", {{"chargerId", "1"}});
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), id);
        QVERIFY(!finished.first().at(2).toBool());
        QCOMPARE(finished.first().at(4).toMap().value("code").toString(), QStringLiteral("UNAVAILABLE"));
        QVERIFY(transport.calls.isEmpty());
    }
};
QTEST_GUILESS_MAIN(WorkflowBridgeTest)
#include "tst_workflow_bridge.moc"
