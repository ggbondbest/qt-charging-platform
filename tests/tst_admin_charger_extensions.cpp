#include "admin_repository.h"
#include "admin_service.h"
#include "billing_service.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "database_connection.h"
#include "user_api_repository.h"
#include "user_api_service.h"
#include "charging/common/model/model_json.h"

#include <QJsonArray>
#include <QSqlQuery>
#include <QtTest>
#include <memory>

using namespace charging::server;
namespace {
QJsonObject data(const QJsonObject& response) { return response.value("data").toObject(); }
QJsonObject item(const QJsonObject& response) { return data(response).value("item").toObject(); }
QString code(const QJsonObject& response) { return response.value("error").toObject().value("code").toString(); }
bool ok(const QJsonObject& response) { return response.value("success").toBool(); }
QJsonObject recovery(const QJsonObject& event, const QString& operation)
{
    return {{"id", event.value("id")}, {"operationId", operation},
            {"expectedUpdatedAt", event.value("updatedAt")}, {"recoveryAction", "SIMULATE_RESTORE"}};
}
} // namespace

class AdminChargerExtensionsTest final : public QObject
{
    Q_OBJECT
    DatabaseConnection db_;
    std::unique_ptr<AdminRepository> repository_;
    std::unique_ptr<AdminService> service_;
    QString token_;
    QJsonObject call(const QString& action, const QJsonObject& parameters = {})
    {
        return service_->handle(action, parameters, token_);
    }
    QJsonObject charger(const QString& id = "1") { return item(call("chargers.get", {{"id", id}})); }
    QJsonObject setState(const QString& id, const QString& status, const QString& operation)
    {
        const auto current = charger(id);
        return call("charger.status", {{"id", id}, {"status", status}, {"operationId", operation},
                                        {"expectedUpdatedAt", current.value("updatedAt")}});
    }
    bool sql(const QString& text) { QSqlQuery query(db_.database()); return query.exec(text); }
    qint64 scalar(const QString& text)
    {
        QSqlQuery query(db_.database());
        return query.exec(text) && query.next() ? query.value(0).toLongLong() : -1;
    }
private slots:
    void maintenanceDtoAndLegacyActionsCannotReleaseAcceptedRepair()
    {
        QVERIFY(ok(setState("1", "FAULT", "fault-before-repair")));
        const auto event = charger().value("activeException").toObject();
        QVERIFY(!event.isEmpty());
        QVERIFY(sql("INSERT INTO repair_reports(user_id,charger_id,problem_type,description,status,created_at,updated_at) "
                    "VALUES(1,1,'CONNECTOR','锁扣松动','ACCEPTED','2026-09-09T10:00:00.000Z','2026-09-09T10:00:00.000Z')"));
        QVERIFY(sql("UPDATE chargers SET status='OFFLINE' WHERE id=1"));
        const auto current = charger();
        QVERIFY(current.value("maintenance").toBool());
        QCOMPARE(current.value("displayStatus").toString(), QStringLiteral("维护中"));
        QCOMPARE(code(call("charger.restart", {{"id", "1"}, {"operationId", "bypass-restart"},
                                               {"expectedUpdatedAt", current.value("updatedAt")}})), QString("RESOURCE_BUSY"));
        QCOMPARE(code(setState("1", "FAULT", "bypass-status")), QString("RESOURCE_BUSY"));
        QCOMPARE(code(call("charger_exceptions.recover", recovery(event, "bypass-recover"))), QString("RESOURCE_BUSY"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM chargers WHERE id=1 AND status='OFFLINE'"), 1LL);
        QCOMPARE(scalar("SELECT COUNT(*) FROM charger_exceptions WHERE id=" + event.value("id").toString() + " AND status='ACTIVE'"), 1LL);
        UserApiRepository userRepository(db_.database());
        UserApiService userService(&userRepository);
        const auto response = userService.handle("GET_CHARGERS", {{"stationId", "1"}}, 1);
        QVERIFY(response.success);
        const auto chargers = response.data.value("chargers").toArray();
        QVERIFY(!chargers.isEmpty());
        const auto first = chargers.first().toObject();
        QVERIFY(first.value("maintenance").isBool());
        QVERIFY(first.value("maintenance").toBool());
        charging::model::Charger parsed;
        QVERIFY(charging::model::fromJson(first, &parsed));
        QVERIFY(parsed.maintenance);
        auto legacy = first; legacy.remove("maintenance");
        QVERIFY(charging::model::fromJson(legacy, &parsed));
        QVERIFY(!parsed.maintenance);
        legacy.insert("maintenance", 1);
        QVERIFY(!charging::model::fromJson(legacy, &parsed));
    }
    void init()
    {
        QString error;
        QVERIFY2(db_.open(":memory:", true, &error), qPrintable(error));
        repository_ = std::make_unique<AdminRepository>(db_.database());
        service_ = std::make_unique<AdminService>(repository_.get());
        token_ = data(service_->handle("auth.login", {{"username", "admin"}, {"password", "123456"}}))
                     .value("sessionToken").toString();
        QVERIFY(!token_.isEmpty());
    }
    void cleanup()
    {
        service_.reset();
        repository_.reset();
        db_.close();
    }
    void authenticationAndStrictValidation()
    {
        for (const auto& action : {"chargers.runtime.get", "charger_exceptions.list", "charger_exceptions.get", "charger_exceptions.recover"})
            QCOMPARE(code(service_->handle(action, {}, "forged")), QString("UNAUTHORIZED"));
        QCOMPARE(code(call("chargers.runtime.get", {{"id", 1}})), QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("chargers.runtime.get", {{"id", "01"}})), QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("chargers.runtime.get", {{"id", "999999"}})), QString("NOT_FOUND"));
        QCOMPARE(code(call("charger_exceptions.get", {{"id", "999999"}})), QString("NOT_FOUND"));
        for (const auto& p : {QJsonObject{{"page", 0}}, QJsonObject{{"pageSize", 101}},
                             QJsonObject{{"status", "FAULT"}}, QJsonObject{{"severity", "secret"}},
                             QJsonObject{{"rawPayload", true}}, QJsonObject{{"occurredAtFrom", "2026-09-01"}},
                             QJsonObject{{"occurredAtFrom", "2026-09-01T00:00:00.000Z"},
                                         {"occurredAtTo", "2026-09-01T00:00:00.000Z"}}})
            QCOMPARE(code(call("charger_exceptions.list", p)), QString("INVALID_ARGUMENT"));
    }
    void oldStatesDoNotInventEventsOrHeartbeat()
    {
        QVERIFY(sql("UPDATE chargers SET status='FAULT',updated_at='2026-01-01T00:00:00.000Z' WHERE id=1"));
        QVERIFY(charger().value("activeException").isNull());
        QCOMPARE(data(call("charger_exceptions.list")).value("total").toInt(), 0);
        const auto runtime = item(call("chargers.runtime.get", {{"id", "1"}}));
        QCOMPARE(runtime.value("status").toString(), QString("FAULT"));
        for (const auto& field : {"sessionId", "capturedAt", "currentPowerWatts", "energyWh", "chargeSeconds", "currentAmountCents", "lastHeartbeatAt"})
            QVERIFY2(runtime.value(field).isNull(), field);
    }
    void recoveryIsVersionedIdempotentAndAudited()
    {
        QVERIFY(ok(setState("1", "FAULT", "fault-one")));
        const auto event = charger().value("activeException").toObject();
        QVERIFY(!event.isEmpty());
        QCOMPARE(event.value("code").toString(), QString("SIMULATED_FAULT"));
        QCOMPARE(event.value("severity").toString(), QString("CRITICAL"));
        QVERIFY(event.value("acknowledgedAt").isNull());
        QVERIFY(event.value("recoveredAt").isNull());
        QCOMPARE(item(call("charger_exceptions.get", {{"id", event.value("id")}})), event);
        QCOMPARE(code(call("charger.restart", {{"id", "1"}, {"operationId", "cannot-bypass-event"},
                                                 {"expectedUpdatedAt", charger().value("updatedAt")}})),
                 QString("INVALID_STATE_TRANSITION"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='charger.restart'"), qint64(0));
        const auto request = recovery(event, "recover-one");
        auto stale = request;
        stale.insert("expectedUpdatedAt", "2025-01-01T00:00:00.000Z");
        QCOMPARE(code(call("charger_exceptions.recover", stale)), QString("CONFLICT"));
        const auto result = call("charger_exceptions.recover", request);
        QVERIFY(ok(result));
        QCOMPARE(item(result).value("status").toString(), QString("RECOVERED"));
        QCOMPARE(item(result).value("occurredAt"), event.value("occurredAt"));
        QVERIFY(item(result).value("acknowledgedAt").isNull());
        QVERIFY(item(result).value("recoveredAt").isString());
        QVERIFY(item(result).value("updatedAt") != event.value("updatedAt"));
        QCOMPARE(item(result).value("recoveredByAdminId").toString(), QString("1"));
        QCOMPARE(item(result).value("recoveryCommandId"), data(result).value("commandId"));
        QVERIFY(data(result).value("simulated").toBool());
        QVERIFY(item(result).value("recoveryMessage").toString().contains(QStringLiteral("未发送硬件命令")));
        QCOMPARE(charger().value("status").toString(), QString("AVAILABLE"));
        QVERIFY(charger().value("activeException").isNull());
        const auto replay = call("charger_exceptions.recover", request);
        QVERIFY(ok(replay));
        QVERIFY(data(replay).value("idempotent").toBool());
        QCOMPARE(item(replay), item(result));
        QCOMPARE(data(replay).value("commandId"), data(result).value("commandId"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='charger_exceptions.recover'"), qint64(1));
        auto reusedOperation = recovery(item(result), "recover-one");
        QCOMPARE(code(call("charger_exceptions.recover", reusedOperation)), QString("CONFLICT"));
        QCOMPARE(code(call("charger_exceptions.recover", recovery(item(result), "recover-again"))), QString("INVALID_STATE_TRANSITION"));
    }
    void recoveryAuditFailureRollsBackEveryWrite()
    {
        QVERIFY(ok(setState("1", "FAULT", "fault-rollback")));
        const auto before = charger();
        const auto event = before.value("activeException").toObject();
        QVERIFY(sql("CREATE TRIGGER reject_recovery_audit BEFORE INSERT ON operation_logs WHEN "
                    "NEW.action='charger_exceptions.recover' BEGIN SELECT RAISE(ABORT,'fixture'); END"));
        const auto request = recovery(event, "recovery-audit-fails");
        QVERIFY(!ok(call("charger_exceptions.recover", request)));
        QCOMPARE(charger(), before);
        QCOMPARE(item(call("charger_exceptions.get", {{"id", event.value("id")}})), event);
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='charger_exceptions.recover'"), qint64(0));
        QVERIFY(sql("DROP TRIGGER reject_recovery_audit"));
        QVERIFY(ok(call("charger_exceptions.recover", request)));
    }
    void statusAuditFailureDoesNotLeaveAnEvent()
    {
        const auto before = charger();
        QVERIFY(sql("CREATE TRIGGER reject_status_audit BEFORE INSERT ON operation_logs WHEN "
                    "NEW.action='charger.status' BEGIN SELECT RAISE(ABORT,'fixture'); END"));
        QVERIFY(!ok(setState("1", "FAULT", "fault-audit-fails")));
        QCOMPARE(charger(), before);
        QCOMPARE(scalar("SELECT COUNT(*) FROM charger_exceptions"), qint64(0));
    }
    void eventClockIsIndependentOfChargerVersion()
    {
        QVERIFY(sql("UPDATE chargers SET updated_at='2099-01-01T00:00:00.000Z' WHERE id=1"));
        const auto before = QDateTime::currentDateTimeUtc();
        QVERIFY(ok(setState("1", "FAULT", "future-version")));
        const auto value = charger();
        const auto event = value.value("activeException").toObject();
        const auto occurred = QDateTime::fromString(event.value("occurredAt").toString(), Qt::ISODateWithMs);
        QVERIFY(occurred >= before && occurred <= QDateTime::currentDateTimeUtc());
        QVERIFY(event.value("occurredAt") != value.value("updatedAt"));
        const auto recovered = call("charger_exceptions.recover", recovery(event, "future-recovery"));
        QVERIFY(ok(recovered));
        const auto recoveredTime = QDateTime::fromString(item(recovered).value("recoveredAt").toString(), Qt::ISODateWithMs);
        QVERIFY(recoveredTime >= before && recoveredTime <= QDateTime::currentDateTimeUtc());
    }
    void busyAndDisallowedStates()
    {
        QVERIFY(ok(setState("1", "FAULT", "fault-state")));
        const auto event = charger().value("activeException").toObject();
        const auto request = recovery(event, "recover-state");
        QVERIFY(sql("UPDATE chargers SET status='CHARGING' WHERE id=1"));
        QCOMPARE(code(call("charger_exceptions.recover", request)), QString("RESOURCE_BUSY"));
        QVERIFY(sql("UPDATE chargers SET status='FAULT' WHERE id=1"));
        QVERIFY(sql("UPDATE stations SET status='INACTIVE' WHERE id=1"));
        QCOMPARE(code(call("charger_exceptions.recover", request)), QString("INVALID_STATE_TRANSITION"));
        QVERIFY(sql("UPDATE stations SET status='ACTIVE' WHERE id=1"));
        QVERIFY(sql("UPDATE charger_exceptions SET status='RECOVERING'"));
        QCOMPARE(code(call("charger_exceptions.recover", request)), QString("INVALID_STATE_TRANSITION"));
        QVERIFY(sql("UPDATE charger_exceptions SET status='ACKNOWLEDGED',acknowledged_at='2026-09-01T00:00:00.000Z'"));
        const auto recovered = call("charger_exceptions.recover", request);
        QVERIFY(ok(recovered));
        QCOMPARE(item(recovered).value("acknowledgedAt").toString(), QString("2026-09-01T00:00:00.000Z"));
    }
    void paginationFiltersAndIndependentHistory()
    {
        QVERIFY(ok(setState("1", "FAULT", "list-fault")));
        QVERIFY(ok(setState("1", "OFFLINE", "list-offline")));
        QVERIFY(ok(setState("2", "FAULT", "list-second")));
        QVERIFY(sql("UPDATE charger_exceptions SET occurred_at='2026-09-01T00:00:00.000Z'"));
        auto listed = data(call("charger_exceptions.list", {{"page", 1}, {"pageSize", 1}}));
        QCOMPARE(listed.value("total").toInt(), 3);
        QCOMPARE(listed.value("items").toArray().first().toObject().value("id").toString(), QString("3"));
        listed = data(call("charger_exceptions.list", {{"page", 2}, {"pageSize", 1}, {"chargerId", "1"}}));
        QCOMPARE(listed.value("total").toInt(), 2);
        QCOMPARE(listed.value("items").toArray().first().toObject().value("id").toString(), QString("1"));
        listed = data(call("charger_exceptions.list", {{"chargerId", "1"}, {"severity", "CRITICAL"}, {"status", "ACTIVE"},
                    {"occurredAtFrom", "2026-09-01T00:00:00.000Z"}, {"occurredAtTo", "2026-09-01T00:00:00.001Z"}}));
        QCOMPARE(listed.value("total").toInt(), 1);
        listed = data(call("charger_exceptions.list", {{"occurredAtTo", "2026-09-01T00:00:00.000Z"}}));
        QCOMPARE(listed.value("total").toInt(), 0);
        QVERIFY(listed.value("items").toArray().isEmpty());
        auto latest = charger().value("activeException").toObject();
        QCOMPARE(latest.value("id").toString(), QString("2"));
        QVERIFY(ok(call("charger_exceptions.recover", recovery(latest, "recover-latest"))));
        QCOMPARE(charger().value("status").toString(), QString("OFFLINE"));
        latest = charger().value("activeException").toObject();
        QCOMPARE(latest.value("id").toString(), QString("1"));
        QVERIFY(ok(call("charger_exceptions.recover", recovery(latest, "recover-last"))));
        QCOMPARE(charger().value("status").toString(), QString("AVAILABLE"));
    }
    void runtimeReadsPersistedSampleAndFrozenTariff()
    {
        ChargingRepository repository(db_.database());
        BillingService billing;
        auto now = QDateTime::currentDateTimeUtc();
        ChargingService charging(&repository, &billing, [&now] { return now; });
        const auto reserved = charging.reserve(1, 1);
        QVERIFY(reserved.success);
        const auto started = charging.startCharging(1, reserved.reservation.id);
        QVERIFY(started.success);
        auto runtime = item(call("chargers.runtime.get", {{"id", "1"}}));
        QCOMPARE(runtime.value("sessionId").toString(), QString::number(started.order.id));
        QCOMPARE(runtime.value("capturedAt").toString(), now.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(runtime.value("lastHeartbeatAt").isNull());
        now = now.addSecs(120);
        const auto sampled = charging.chargingStatus(1, started.order.id);
        QVERIFY(sampled.success);
        runtime = item(call("chargers.runtime.get", {{"id", "1"}}));
        QCOMPARE(runtime.value("capturedAt").toString(), now.toUTC().toString(Qt::ISODateWithMs));
        QCOMPARE(runtime.value("currentPowerWatts").toInteger(), qint64(sampled.currentPowerWatts));
        QCOMPARE(runtime.value("energyWh").toInteger(), sampled.order.energyWh);
        QCOMPARE(runtime.value("chargeSeconds").toInteger(), qint64(120));
        QCOMPARE(runtime.value("currentAmountCents").toInteger(), sampled.order.amountCents);
        QVERIFY(runtime.value("estimated").toBool());
        QVERIFY(sql("UPDATE stations SET price_cents_per_kwh=99999 WHERE id=1"));
        QVERIFY(sql("UPDATE orders SET amount_cents=999999 WHERE status='CHARGING'"));
        QCOMPARE(item(call("chargers.runtime.get", {{"id", "1"}})), runtime);
        // Reading repeatedly does not synthesize a later sampling timestamp.
        now = now.addSecs(60);
        QCOMPARE(item(call("chargers.runtime.get", {{"id", "1"}})), runtime);
        QVERIFY(charging.stopCharging(1, started.order.id).success);
        runtime = item(call("chargers.runtime.get", {{"id", "1"}}));
        for (const auto& field : {"sessionId", "capturedAt", "currentPowerWatts", "energyWh", "chargeSeconds", "currentAmountCents", "lastHeartbeatAt"})
            QVERIFY2(runtime.value(field).isNull(), field);
    }
    void legacyRuntimeDoesNotInventTariffSnapshot()
    {
        QVERIFY(sql("INSERT INTO orders(order_no,user_id,charger_id,status,unit_price_cents_per_kwh,"
                    "energy_wh,duration_seconds,amount_cents,started_at,telemetry_captured_at,telemetry_power_watts) "
                    "VALUES('LEGACY-RUNTIME',1,1,'CHARGING',120,2000,60,240,'2026-09-01T00:00:00.000Z',"
                    "'2026-09-01T00:01:00.000Z',120000)"));
        QVERIFY(sql("UPDATE chargers SET status='CHARGING' WHERE id=1"));
        const auto runtime = item(call("chargers.runtime.get", {{"id", "1"}}));
        QCOMPARE(runtime.value("energyWh").toInt(), 2000);
        QCOMPARE(runtime.value("capturedAt").toString(), QString("2026-09-01T00:01:00.000Z"));
        QVERIFY(runtime.value("currentAmountCents").isNull());
        QVERIFY(runtime.value("lastHeartbeatAt").isNull());
    }
};

QTEST_GUILESS_MAIN(AdminChargerExtensionsTest)
#include "tst_admin_charger_extensions.moc"
