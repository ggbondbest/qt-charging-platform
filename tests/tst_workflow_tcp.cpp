#include "server_runtime.h"
#include "database_connection.h"
#include "charging/common/protocol/frame_codec.h"
#include "charging/common/protocol/protocol.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace charging::protocol;
using charging::server::DatabaseConnection;
using charging::server::ServerRuntime;

namespace {
class Peer final
{
public:
    QTcpSocket socket;
    FrameDecoder decoder;
    QMap<QString, ResponseEnvelope> responses;
    QList<EventEnvelope> events;
    QString error;
    int sequence = 0;
    bool connectTo(quint16 port)
    {
        socket.connectToHost(QHostAddress::LocalHost, port);
        return socket.waitForConnected(2000);
    }
    bool drain(int timeout = 100)
    {
        if (!socket.bytesAvailable()) socket.waitForReadyRead(timeout);
        QList<QByteArray> payloads;
        if (!decoder.append(socket.readAll(), &payloads)) { error = "Invalid frame"; return false; }
        for (const auto& payload : payloads) {
            const auto json = QJsonDocument::fromJson(payload).object();
            if (json.value("kind").toString() == "EVENT") {
                EventEnvelope event;
                if (!parseEventPayload(payload, &event)) { error = "Invalid event"; return false; }
                // Push carries only invalidation metadata, not anyone's rows,
                // phone, balance, descriptions, session token or order id.
                if (event.type != "WORKFLOW_CHANGED" || json.contains("requestId")) {
                    error = "Invalid event correlation"; return false;
                }
                events.append(event);
            } else {
                ResponseEnvelope response;
                if (!parseResponsePayload(payload, &response)) { error = "Invalid response"; return false; }
                responses.insert(response.requestId, response);
            }
        }
        return true;
    }
    ResponseEnvelope exchange(const QString& type, const QJsonObject& data = {})
    {
        RequestEnvelope request;
        request.type = type;
        request.requestId = QStringLiteral("tcp-%1").arg(++sequence);
        request.data = data;
        QByteArray frame;
        if (!encodeFrame(serializePayload(request), &frame)) return {};
        socket.write(frame);
        socket.waitForBytesWritten(1000);
        QElapsedTimer timer; timer.start();
        while (!responses.contains(request.requestId) && timer.elapsed() < 5000)
            if (!drain(100)) return {};
        const auto result = responses.take(request.requestId);
        if (result.requestId != request.requestId || result.type != type) error = "Response correlation lost";
        return result;
    }
    bool login(const QString& phone)
    { return exchange(QStringLiteral("USER_LOGIN"), {{QStringLiteral("phone"), phone}}).success; }
};

class Fixture final
{
public:
    QTemporaryDir directory;
    ServerRuntime runtime;
    DatabaseConnection observer;
    QString error;
    int adminSequence = 0;
    QString adminToken;
    bool start()
    {
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        if (!runtime.start(directory.filePath("workflow.sqlite3"), true, QHostAddress::LocalHost, 0)) return false;
        QElapsedTimer timer; timer.start();
        while (ready.isEmpty() && timer.elapsed() < 5000) QTest::qWait(10);
        if (ready.isEmpty()) return false;
        return observer.open(directory.filePath("workflow.sqlite3"), false, &error);
    }
    QVariant value(const QString& sql)
    {
        QSqlQuery q(observer.database());
        return q.exec(sql) && q.next() ? q.value(0) : QVariant{};
    }
    bool exec(const QString& sql)
    { QSqlQuery q(observer.database()); return q.exec(sql); }
    QJsonObject admin(const QString& action, const QJsonObject& data = {})
    {
        const QString id = QStringLiteral("workflow-admin-%1").arg(++adminSequence);
        QSignalSpy responses(&runtime, &ServerRuntime::adminResponse);
        runtime.submitAdminRequest(id, action, data, adminToken, QDateTime::currentMSecsSinceEpoch() + 5000);
        QElapsedTimer timer; timer.start();
        while (responses.isEmpty() && timer.elapsed() < 5000) QTest::qWait(10);
        for (const auto& row : responses) if (row[0].toString() == id) return row[1].toJsonObject();
        return {};
    }
    bool adminLogin()
    {
        const auto result = admin("auth.login", {{"username", "admin"}, {"password", "123456"}});
        adminToken = result.value("data").toObject().value("sessionToken").toString();
        return result.value("success").toBool() && !adminToken.isEmpty();
    }
};

QJsonObject reportItem(const QJsonObject& reply)
{ return reply.value("data").toObject().value("item").toObject(); }
}

class WorkflowTcpTest final : public QObject
{
    Q_OBJECT
private slots:
    void anonymousRequestsStayUnauthorized()
    {
        Fixture f;
        QVERIFY2(f.start(), qPrintable(f.error));
        Peer peer;
        QVERIFY(peer.connectTo(f.runtime.serverPort()));
        for (const auto& type : {"WORKFLOW_SUBSCRIBE", "QUEUE_GET_MINE", "QUEUE_JOIN",
                                 "REPAIR_GET_MINE", "REPAIR_SUBMIT"}) {
            const auto response = peer.exchange(type);
            QVERIFY(!response.success);
            QCOMPARE(response.error.code, QStringLiteral("UNAUTHORIZED"));
        }
        QVERIFY(peer.error.isEmpty());
        QTest::qWait(1100);
        QVERIFY(peer.drain(30));
        QVERIFY(peer.events.isEmpty());
        QVERIFY(!f.admin("queues.list").value("success").toBool());
        QVERIFY(!f.admin("repair_reports.list").value("success").toBool());
    }

    void queueCallTimeoutAndConfirmationCrossClients()
    {
        Fixture f;
        QVERIFY2(f.start(), qPrintable(f.error));
        // Exactly one usable charger at the selected station. These are
        // isolated fixture edits, never the application's configured DB.
        QVERIFY(f.exec("UPDATE chargers SET status='OFFLINE' WHERE station_id=1 AND id<>1"));
        Peer a, b, c;
        QVERIFY(a.connectTo(f.runtime.serverPort()) && b.connectTo(f.runtime.serverPort()) && c.connectTo(f.runtime.serverPort()));
        QVERIFY(a.login("13800138000") && b.login("13900139000") && c.login("13700137000"));
        QVERIFY(b.exchange("WORKFLOW_SUBSCRIBE").success);
        QVERIFY(c.exchange("WORKFLOW_SUBSCRIBE").success);
        const auto reserved = a.exchange("RESERVE_CHARGER", {{"chargerId", "1"}});
        QVERIFY(reserved.success);
        const auto started = a.exchange("START_CHARGING", {{"reservationId", reserved.data["reservation"].toObject()["id"]}});
        QVERIFY(started.success);
        const auto bJoined = b.exchange("QUEUE_JOIN", {{"chargerId", "1"}, {"operationId", "tcp_b_join_001"}});
        const auto cJoined = c.exchange("QUEUE_JOIN", {{"chargerId", "1"}, {"operationId", "tcp_c_join_001"}});
        QVERIFY2(bJoined.success, qPrintable(bJoined.error.message));
        QVERIFY2(cJoined.success, qPrintable(cJoined.error.message));
        const auto bId = bJoined.data.value("item").toObject().value("id").toString();
        const auto cId = cJoined.data.value("item").toObject().value("id").toString();
        QCOMPARE(cJoined.data.value("item").toObject().value("aheadCount").toInt(), 1);
        QVERIFY(a.exchange("STOP_CHARGING", {{"orderId", started.data["order"].toObject()["id"]}}).success);
        QTRY_COMPARE_WITH_TIMEOUT(f.value("SELECT status FROM queue_entries WHERE id=" + bId).toString(), QStringLiteral("CALLED"), 4000);
        QVERIFY(b.drain(100)); QVERIFY(c.drain(100));
        QVERIFY(!b.events.isEmpty()); QVERIFY(!c.events.isEmpty());
        QCOMPARE(b.exchange("QUEUE_GET_MINE").data["item"].toObject()["status"].toString(), QStringLiteral("CALLED"));
        // Advance just the persisted invitation's deadline; this proves the
        // worker's timer (not a client's poll) hands the slot to C.
        const auto now = QDateTime::currentDateTimeUtc();
        QSqlQuery expiry(f.observer.database());
        expiry.prepare("UPDATE queue_entries SET called_at=?,call_expires_at=? WHERE id=?");
        expiry.addBindValue(now.addSecs(-90).toString(Qt::ISODateWithMs));
        expiry.addBindValue(now.addSecs(-1).toString(Qt::ISODateWithMs));
        expiry.addBindValue(bId);
        QVERIFY(expiry.exec()); expiry.finish();
        QTRY_COMPARE_WITH_TIMEOUT(f.value("SELECT status FROM queue_entries WHERE id=" + cId).toString(), QStringLiteral("CALLED"), 4000);
        QCOMPARE(f.value("SELECT status FROM queue_entries WHERE id=" + bId).toString(), QStringLiteral("EXPIRED"));
        QVERIFY(!b.exchange("QUEUE_CONFIRM", {{"id", cId}, {"operationId", "tcp_steal_001"}}).success);
        const auto confirmed = c.exchange("QUEUE_CONFIRM", {{"id", cId}, {"operationId", "tcp_c_confirm_001"}});
        QVERIFY2(confirmed.success, qPrintable(confirmed.error.message));
        QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QStringLiteral("RESERVED"));
        QCOMPARE(f.value("SELECT COUNT(*) FROM reservations WHERE charger_id=1 AND status='ACTIVE'").toInt(), 1);
        QVERIFY(f.adminLogin());
        const auto list = f.admin("queues.list", {{"stationId", "1"}, {"status", "ALL"}});
        QVERIFY(list.value("success").toBool());
        QCOMPARE(list.value("data").toObject().value("total").toInt(), 2);
        QVERIFY(a.drain(20));
        QVERIFY(a.events.isEmpty()); // Legacy sessions never opted in.
        QVERIFY(a.error.isEmpty() && b.error.isEmpty() && c.error.isEmpty());
    }

    void repairProgressAndMaintenanceAreVisibleAcrossClients()
    {
        Fixture f;
        QVERIFY2(f.start(), qPrintable(f.error));
        Peer reporter, viewer;
        QVERIFY(reporter.connectTo(f.runtime.serverPort()) && viewer.connectTo(f.runtime.serverPort()));
        QVERIFY(reporter.login("13800138000") && viewer.login("13900139000"));
        QVERIFY(reporter.exchange("WORKFLOW_SUBSCRIBE").success);
        QVERIFY(viewer.exchange("WORKFLOW_SUBSCRIBE").success);
        const auto submitted = reporter.exchange("REPAIR_SUBMIT", {{"chargerId", "1"},
            {"problemType", "CONNECTOR"}, {"description", "连接器锁扣松动，请核实"}, {"operationId", "tcp_report_001"}});
        QVERIFY2(submitted.success, qPrintable(submitted.error.message));
        auto report = submitted.data.value("item").toObject();
        QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QStringLiteral("AVAILABLE"));
        QVERIFY(!viewer.exchange("REPAIR_GET", {{"id", report.value("id")}}).success);
        QVERIFY(f.adminLogin());
        const QStringList actions{"repair_reports.accept", "repair_reports.start", "repair_reports.resolve"};
        for (int step = 0; step < actions.size(); ++step) {
            const auto reply = f.admin(actions[step], {{"id", report.value("id")},
                {"expectedUpdatedAt", report.value("updatedAt")}, {"operationId", QStringLiteral("tcp_repair_%1").arg(step)},
                {"note", QStringLiteral("核实并模拟维修步骤 %1").arg(step)}});
            QVERIFY2(reply.value("success").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
            report = reportItem(reply);
            if (step == 0) {
                QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QStringLiteral("OFFLINE"));
                const auto chargers = viewer.exchange("GET_CHARGERS", {{"stationId", "1"}});
                QVERIFY(chargers.success);
                bool found = false;
                for (const auto& row : chargers.data.value("chargers").toArray()) {
                    const auto charger = row.toObject();
                    if (charger.value("id").toString() == "1") {
                        QVERIFY(charger.value("maintenance").toBool());
                        found = true;
                    }
                }
                QVERIFY(found);
                QVERIFY(!viewer.exchange("RESERVE_CHARGER", {{"chargerId", "1"}}).success);
            }
        }
        QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QStringLiteral("AVAILABLE"));
        const auto traced = reporter.exchange("REPAIR_GET", {{"id", report.value("id")}});
        QVERIFY(traced.success);
        QCOMPARE(traced.data.value("item").toObject().value("status").toString(), QStringLiteral("RESOLVED"));
        QCOMPARE(traced.data.value("item").toObject().value("timeline").toArray().size(), 4);
        QTest::qWait(1100);
        QVERIFY(reporter.drain(100) && viewer.drain(100));
        QVERIFY(!reporter.events.isEmpty() && !viewer.events.isEmpty());
        QCOMPARE(f.value("SELECT COUNT(*) FROM notifications WHERE type='REPAIR_UPDATED'").toInt(), 3);
    }

    void disconnectedTargetUserStillStopsAndOldClientGetsNoEvents()
    {
        Fixture f;
        QVERIFY2(f.start(), qPrintable(f.error));
        Peer owner, oldClient, subscriber;
        QVERIFY(owner.connectTo(f.runtime.serverPort()) && oldClient.connectTo(f.runtime.serverPort())
                && subscriber.connectTo(f.runtime.serverPort()));
        QVERIFY(owner.login("13800138000") && oldClient.login("13900139000") && subscriber.login("13700137000"));
        QVERIFY(subscriber.exchange("WORKFLOW_SUBSCRIBE").success);
        const auto reserved = owner.exchange("RESERVE_CHARGER", {{"chargerId", "1"}});
        QVERIFY(reserved.success);
        // Own the ID: operator[] on a temporary QJsonObject returns a dangling
        // QJsonValueRef once the statement ends (including on Qt 6.2.4).
        const QString reservationId = reserved.data.value("reservation").toObject().value("id").toString();
        QVERIFY(!reservationId.isEmpty());
        const auto invalidTarget = owner.exchange("START_CHARGING", {{"reservationId", reservationId}, {"target", QJsonObject{}}});
        QVERIFY(!invalidTarget.success);
        QCOMPARE(invalidTarget.error.code, QStringLiteral("INVALID_ARGUMENT"));
        const auto started = owner.exchange("START_CHARGING", {{"reservationId", reservationId},
            {"target", QJsonObject{{"type", "AMOUNT"}, {"value", 1}}}});
        QVERIFY2(started.success, qPrintable(started.error.message));
        const auto orderId = started.data.value("order").toObject().value("id").toString();
        owner.socket.abort();
        QTRY_COMPARE_WITH_TIMEOUT(f.value("SELECT status FROM orders WHERE id=" + orderId).toString(), QStringLiteral("WAITING_PAYMENT"), 5000);
        QCOMPARE(f.value("SELECT stop_reason FROM orders WHERE id=" + orderId).toString(), QStringLiteral("TARGET_AMOUNT"));
        QVERIFY(f.value("SELECT amount_cents FROM orders WHERE id=" + orderId).toInt() <= 1);
        QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QStringLiteral("AVAILABLE"));
        QVERIFY(subscriber.drain(200));
        QVERIFY(!subscriber.events.isEmpty());
        for (const auto& event : subscriber.events) {
            const auto payload = QJsonDocument(event.data).toJson(QJsonDocument::Compact);
            QVERIFY(!payload.contains("userId") && !payload.contains("phone") && !payload.contains("orderId")
                    && !payload.contains("token") && !payload.contains("description"));
        }
        QVERIFY(oldClient.drain(100));
        QVERIFY(oldClient.events.isEmpty());
        // An event queued just before a response cannot be mistaken for it.
        const auto profile = subscriber.exchange("GET_USER_INFO");
        QVERIFY(profile.success);
        QVERIFY(subscriber.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(WorkflowTcpTest)
#include "tst_workflow_tcp.moc"
