#include "billing_service.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "database_connection.h"
#include "queue_repository.h"
#include "queue_service.h"

#include <QJsonArray>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace charging::server;
namespace {
QJsonObject body(const QJsonObject& reply) { return reply.value("data").toObject(); }
QJsonObject item(const QJsonObject& reply) { return body(reply).value("item").toObject(); }
bool ok(const QJsonObject& reply) { return reply.value("success").toBool(); }
QString errorCode(const QJsonObject& reply) { return reply.value("error").toObject().value("code").toString(); }

class Fixture final
{
public:
    QTemporaryDir directory;
    DatabaseConnection connection;
    BillingService billing;
    std::unique_ptr<ChargingRepository> chargingRepository;
    std::unique_ptr<ChargingService> charging;
    std::unique_ptr<QueueRepository> repository;
    std::unique_ptr<QueueService> queue;
    QDateTime now = QDateTime::fromString("2026-09-09T08:00:00.000Z", Qt::ISODateWithMs);
    qint64 order = 0;
    QString diagnostic;
    bool exec(const QString& sql)
    {
        QSqlQuery q(connection.database());
        if (!q.exec(sql)) { diagnostic = q.lastError().text(); return false; }
        return true;
    }
    QVariant value(const QString& sql)
    {
        QSqlQuery q(connection.database());
        if (!q.exec(sql) || !q.next()) { diagnostic = q.lastError().text(); return {}; }
        return q.value(0);
    }
    void services()
    {
        chargingRepository = std::make_unique<ChargingRepository>(connection.database());
        charging = std::make_unique<ChargingService>(chargingRepository.get(), &billing, [this] { return now; });
        repository = std::make_unique<QueueRepository>(connection.database());
        queue = std::make_unique<QueueService>(repository.get(), [this] { return now; });
    }
    bool start(bool busy = true)
    {
        if (!directory.isValid() || !connection.open(directory.filePath("queue.db"), false, &diagnostic)) return false;
        for (int id = 1; id <= 4; ++id)
            if (!exec(QStringLiteral("INSERT INTO users(id,phone,nickname,balance_cents,status) VALUES (%1,'1380000000%1','用户%1',10000,'ACTIVE')").arg(id))) return false;
        if (!exec("INSERT INTO stations(id,code,name,address,latitude,longitude,price_cents_per_kwh,status) VALUES (1,'QUEUE-STATION','排队测试站','地址',38.9,121.5,120,'ACTIVE')") ||
            !exec("INSERT INTO chargers(id,station_id,code,type,power_watts,status) VALUES (1,1,'QUEUE-CHARGER','FAST',60000,'AVAILABLE')")) return false;
        services();
        if (busy) {
            const auto reserved = charging->reserve(1, 1);
            if (!reserved.success) { diagnostic = reserved.error.message; return false; }
            const auto started = charging->startCharging(1, reserved.reservation.id);
            if (!started.success) { diagnostic = started.error.message; return false; }
            order = started.order.id;
        }
        return true;
    }
    QJsonObject join(qint64 user, const QString& operation = "join_00000001", qint64 charger = 1)
    {
        return queue->handle("QUEUE_JOIN", {{"chargerId", QString::number(charger)}, {"operationId", operation}}, user);
    }
    QJsonObject mine(qint64 user) { return queue->handle("QUEUE_GET_MINE", {}, user); }
    QJsonObject command(const QString& action, qint64 user, const QString& id, const QString& operation)
    {
        return queue->handle(action, {{"id", id}, {"operationId", operation}}, user);
    }
    bool release()
    {
        now = now.addSecs(10);
        const auto stopped = charging->stopCharging(1, order);
        if (!stopped.success) { diagnostic = stopped.error.message; return false; }
        return queue->tick(now, &diagnostic);
    }
    bool reopen()
    {
        queue.reset(); repository.reset(); charging.reset(); chargingRepository.reset();
        const QString path = connection.databasePath(); connection.close();
        if (!connection.open(path, false, &diagnostic)) return false;
        services(); return true;
    }
};
} // namespace

class QueueWorkflowTest final : public QObject
{
    Q_OBJECT
private slots:
    void fifoCallTimeoutThenNextUser();
    void confirmationIsAtomicAndEntersExistingReservationCountdown();
    void callAndWriteReplayDoNotDuplicate();
    void reservationTimeoutCallsNextAndCannotStealSlot();
    void waitingQueuesSurviveRestartAndPauseForMaintenance();
    void notificationFailureRollsBackCall();
    void identityAndJoinEligibility();
    void frozenOrInactiveCalledUserReleasesSlot();
    void administratorListIsPagedAndMasked();
};

void QueueWorkflowTest::fifoCallTimeoutThenNextUser()
{
    Fixture f; QVERIFY2(f.start(), qPrintable(f.diagnostic));
    const auto b = f.join(2), c = f.join(3);
    QVERIFY(ok(b)); QVERIFY(ok(c));
    QCOMPARE(item(b).value("position").toInt(), 1);
    QCOMPARE(item(c).value("position").toInt(), 2);
    QCOMPARE(item(c).value("aheadCount").toInt(), 1);
    QVERIFY2(f.release(), qPrintable(f.diagnostic));
    QCOMPARE(item(f.mine(2)).value("status").toString(), QString("CALLED"));
    QCOMPARE(item(f.mine(3)).value("status").toString(), QString("WAITING"));
    QCOMPARE(f.value("SELECT count(*) FROM notifications WHERE type='QUEUE_CALLED'").toInt(), 1);
    f.now = f.now.addSecs(59); QVERIFY(f.queue->tick(f.now));
    QCOMPARE(item(f.mine(2)).value("confirmationSecondsRemaining").toInt(), 1);
    f.now = f.now.addSecs(1); QVERIFY(f.queue->tick(f.now));
    QCOMPARE(item(f.mine(2)).value("status").toString(), QString("EXPIRED"));
    QCOMPARE(item(f.mine(3)).value("status").toString(), QString("CALLED"));
    QCOMPARE(item(f.mine(3)).value("position").toInt(), 1);
    QCOMPARE(f.value("SELECT count(*) FROM notifications WHERE type='QUEUE_CALLED'").toInt(), 2);
    QCOMPARE(f.value("SELECT count(*) FROM notifications WHERE type='QUEUE_EXPIRED'").toInt(), 1);
}

void QueueWorkflowTest::confirmationIsAtomicAndEntersExistingReservationCountdown()
{
    Fixture f; QVERIFY2(f.start(), qPrintable(f.diagnostic));
    const QString id = item(f.join(2)).value("id").toString();
    QVERIFY(f.release());
    QVERIFY(f.exec("CREATE TRIGGER reject_queue_snapshot BEFORE INSERT ON order_pricing_snapshots BEGIN SELECT RAISE(ABORT,'test snapshot rollback'); END"));
    const auto failed = f.command("QUEUE_CONFIRM", 2, id, "confirm_0001");
    QCOMPARE(errorCode(failed), QString("DATABASE_ERROR"));
    QCOMPARE(f.value("SELECT count(*) FROM reservations WHERE user_id=2").toInt(), 0);
    QCOMPARE(f.value("SELECT count(*) FROM orders WHERE user_id=2").toInt(), 0);
    QCOMPARE(item(f.mine(2)).value("status").toString(), QString("CALLED"));
    QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QString("RESERVED"));
    QVERIFY(f.exec("DROP TRIGGER reject_queue_snapshot"));
    const auto confirmed = f.command("QUEUE_CONFIRM", 2, id, "confirm_0001");
    QVERIFY2(ok(confirmed), qPrintable(QString::fromUtf8(QJsonDocument(confirmed).toJson())));
    QCOMPARE(item(confirmed).value("status").toString(), QString("CONFIRMED"));
    const auto reservation = body(confirmed).value("reservation").toObject();
    const auto expires = QDateTime::fromString(reservation.value("expiresAt").toString(), Qt::ISODateWithMs);
    QCOMPARE(f.now.secsTo(expires), qint64(900));
    QCOMPARE(f.value("SELECT count(*) FROM order_pricing_snapshots").toInt(), 2);
    const auto replay = f.command("QUEUE_CONFIRM", 2, id, "confirm_0001");
    QVERIFY(ok(replay)); QVERIFY(body(replay).value("idempotent").toBool());
    QCOMPARE(body(replay).value("reservation"), body(confirmed).value("reservation"));
    QVERIFY(!ok(f.command("QUEUE_CONFIRM", 2, id, "confirm_0002")));
}

void QueueWorkflowTest::callAndWriteReplayDoNotDuplicate()
{
    Fixture f; QVERIFY(f.start());
    const auto first = f.join(2);
    const auto replay = f.join(2);
    QVERIFY(ok(first)); QVERIFY(ok(replay)); QVERIFY(body(replay).value("idempotent").toBool());
    QCOMPARE(item(first).value("id"), item(replay).value("id"));
    QCOMPARE(f.value("SELECT count(*) FROM queue_entries").toInt(), 1);
    QVERIFY(!ok(f.join(2, "join_00000002")));
    const QString id = item(first).value("id").toString();
    const QString cId = item(f.join(3)).value("id").toString();
    QVERIFY(f.release());
    for (int i = 0; i < 5; ++i) QVERIFY(f.queue->tick(f.now));
    QCOMPARE(f.value("SELECT count(*) FROM notifications WHERE type='QUEUE_CALLED'").toInt(), 1);
    const auto left = f.command("QUEUE_LEAVE", 2, id, "leave_000001");
    QVERIFY(ok(left)); QCOMPARE(item(left).value("status").toString(), QString("LEFT"));
    QVERIFY(body(f.command("QUEUE_LEAVE", 2, id, "leave_000001")).value("idempotent").toBool());
    QCOMPARE(item(f.mine(3)).value("id").toString(), cId);
    QCOMPARE(item(f.mine(3)).value("status").toString(), QString("CALLED"));
    QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QString("RESERVED"));
}

void QueueWorkflowTest::reservationTimeoutCallsNextAndCannotStealSlot()
{
    Fixture f; QVERIFY(f.start());
    const QString id = item(f.join(2)).value("id").toString(); QVERIFY(ok(f.join(3)));
    QVERIFY(f.release());
    QVERIFY(!f.charging->reserve(4, 1).success);
    const auto confirmed = f.command("QUEUE_CONFIRM", 2, id, "confirm_0001"); QVERIFY(ok(confirmed));
    const qint64 reservationId = item(confirmed).value("reservationId").toString().toLongLong();
    f.now = f.now.addSecs(900);
    QVERIFY(f.chargingRepository->expireReservations(f.now)); QVERIFY(f.queue->tick(f.now));
    QCOMPARE(f.value(QString("SELECT status FROM reservations WHERE id=%1").arg(reservationId)).toString(), QString("EXPIRED"));
    QCOMPARE(item(f.mine(3)).value("status").toString(), QString("CALLED"));
    QVERIFY(!f.charging->reserve(4, 1).success);
    QVERIFY(!f.charging->startCharging(2, reservationId).success);
    QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QString("RESERVED"));
}

void QueueWorkflowTest::waitingQueuesSurviveRestartAndPauseForMaintenance()
{
    Fixture f; QVERIFY(f.start());
    QVERIFY(ok(f.join(2))); QVERIFY(ok(f.join(3)));
    QVERIFY2(f.reopen(), qPrintable(f.diagnostic));
    QCOMPARE(item(f.mine(3)).value("aheadCount").toInt(), 1);
    // Simulate a persisted maintenance state after A's charging finishes.
    f.now = f.now.addSecs(10); QVERIFY(f.charging->stopCharging(1, f.order).success);
    QVERIFY(f.exec("INSERT INTO repair_reports(user_id,charger_id,problem_type,description,status,created_at,updated_at) VALUES(4,1,'OTHER','模拟维护','PROCESSING','2026-09-09T08:00:10.000Z','2026-09-09T08:00:10.000Z')"));
    QVERIFY(f.queue->tick(f.now));
    QCOMPARE(item(f.mine(2)).value("status").toString(), QString("WAITING"));
    QVERIFY(item(f.mine(2)).value("maintenance").toBool());
    QVERIFY(!f.charging->reserve(4, 1).success);
    QVERIFY(f.exec("UPDATE repair_reports SET status='RESOLVED'"));
    QVERIFY(f.exec("UPDATE stations SET status='INACTIVE' WHERE id=1"));
    QVERIFY(f.queue->tick(f.now));
    QCOMPARE(item(f.mine(2)).value("status").toString(), QString("WAITING"));
    QVERIFY(f.exec("UPDATE stations SET status='ACTIVE' WHERE id=1"));
    QVERIFY(f.queue->tick(f.now));
    QCOMPARE(item(f.mine(2)).value("status").toString(), QString("CALLED"));
}

void QueueWorkflowTest::notificationFailureRollsBackCall()
{
    Fixture f; QVERIFY(f.start()); QVERIFY(ok(f.join(2)));
    f.now = f.now.addSecs(10); QVERIFY(f.charging->stopCharging(1, f.order).success);
    QVERIFY(f.exec("CREATE TRIGGER reject_queue_notice BEFORE INSERT ON notifications WHEN NEW.type='QUEUE_CALLED' BEGIN SELECT RAISE(ABORT,'test notification rollback'); END"));
    QVERIFY(!f.queue->tick(f.now));
    QCOMPARE(f.value("SELECT status FROM queue_entries").toString(), QString("WAITING"));
    QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QString("AVAILABLE"));
    QVERIFY(f.exec("DROP TRIGGER reject_queue_notice"));
    // Even between release and the maintenance tick, ordinary reservations
    // must not bypass a persisted FIFO queue.
    QVERIFY(!f.charging->reserve(4, 1).success);
    QVERIFY(f.queue->tick(f.now));
    QCOMPARE(item(f.mine(2)).value("status").toString(), QString("CALLED"));
}

void QueueWorkflowTest::identityAndJoinEligibility()
{
    Fixture f; QVERIFY(f.start(false));
    QCOMPARE(errorCode(f.join(2)), QString("CONFLICT"));
    const auto reserved = f.charging->reserve(1, 1); QVERIFY(reserved.success);
    QVERIFY(!ok(f.join(1)));
    QVERIFY(!ok(f.queue->handle("QUEUE_JOIN", {{"chargerId", 1}, {"operationId", "join_00000001"}}, 2)));
    QVERIFY(!ok(f.queue->handle("QUEUE_JOIN", {{"chargerId", "1"}, {"userId", "3"}, {"operationId", "join_00000001"}}, 2)));
    QVERIFY(!ok(f.queue->handle("QUEUE_GET_MINE", {}, 0)));
    const QString id = item(f.join(2)).value("id").toString(); QVERIFY(!id.isEmpty());
    QCOMPARE(errorCode(f.command("QUEUE_LEAVE", 3, id, "leave_000001")), QString("NOT_FOUND"));
    QCOMPARE(errorCode(f.command("QUEUE_CONFIRM", 3, id, "confirm_0001")), QString("NOT_FOUND"));
    QVERIFY(!ok(f.command("QUEUE_CONFIRM", 2, id, "confirm_0001")));
}

void QueueWorkflowTest::frozenOrInactiveCalledUserReleasesSlot()
{
    Fixture f; QVERIFY(f.start()); QVERIFY(ok(f.join(2))); QVERIFY(ok(f.join(3))); QVERIFY(f.release());
    QVERIFY(f.exec("UPDATE users SET status='FROZEN' WHERE id=2"));
    QVERIFY(f.queue->tick(f.now));
    QCOMPARE(f.value("SELECT status FROM queue_entries WHERE user_id=2").toString(), QString("EXPIRED"));
    QCOMPARE(item(f.mine(3)).value("status").toString(), QString("CALLED"));
    QVERIFY(f.exec("UPDATE stations SET status='INACTIVE' WHERE id=1"));
    QVERIFY(f.queue->tick(f.now));
    QCOMPARE(item(f.mine(3)).value("status").toString(), QString("EXPIRED"));
    QCOMPARE(f.value("SELECT status FROM chargers WHERE id=1").toString(), QString("AVAILABLE"));
}

void QueueWorkflowTest::administratorListIsPagedAndMasked()
{
    Fixture f; QVERIFY(f.start()); QVERIFY(ok(f.join(2))); QVERIFY(ok(f.join(3)));
    const auto first = f.queue->adminRead("queues.list", {{"stationId", "1"}, {"pageSize", 1}, {"page", 1}});
    QVERIFY(ok(first)); QCOMPARE(body(first).value("total").toInt(), 2);
    const auto row = body(first).value("items").toArray().first().toObject();
    QCOMPARE(row.value("userPhoneMasked").toString(), QString("138****0002"));
    QVERIFY(!row.contains("phone")); QVERIFY(!item(f.mine(2)).contains("userPhoneMasked"));
    const auto second = f.queue->adminRead("queues.list", {{"stationId", "1"}, {"pageSize", 1}, {"page", 2}});
    QCOMPARE(body(second).value("items").toArray().first().toObject().value("position").toInt(), 2);
    QVERIFY(!ok(f.queue->adminRead("queues.list", {{"pageSize", 101}})));
    QVERIFY(!ok(f.queue->adminRead("queues.list", {{"status", "BAD"}})));
}

QTEST_GUILESS_MAIN(QueueWorkflowTest)
#include "tst_queue_workflow.moc"
