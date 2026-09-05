#include "admin_repository.h"
#include "admin_service.h"
#include "billing_service.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "database_connection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <memory>
#include <thread>

using namespace charging::server;
namespace {
QString code(const QJsonObject& r)
{
    return r.value("error").toObject().value("code").toString();
}
QJsonObject data(const QJsonObject& r)
{
    return r.value("data").toObject();
}
QJsonObject item(const QJsonObject& r)
{
    return data(r).value("item").toObject();
}
bool ok(const QJsonObject& r)
{
    return r.value("success").toBool();
}
QJsonObject loginData()
{
    return {{"username", "admin"}, {"password", "123456"}};
}
QJsonObject newStation(const QString& operation = "create-1", const QString& stationCode = "NEW-1",
                       const QString& chargerCode = "NEW-C1")
{
    return {{"operationId", operation},
            {"code", stationCode},
            {"name", "测试站"},
            {"address", "测试路1号"},
            {"latitude", 31.2},
            {"longitude", 121.4},
            {"priceCentsPerKwh", 120},
            {"chargers", QJsonArray{QJsonObject{
                             {"code", chargerCode}, {"type", "SLOW"}, {"powerWatts", 7200}}}}};
}
QJsonObject change(const QJsonObject& value, const QString& operation, const QString& status)
{
    return {{"id", value.value("id")},
            {"expectedUpdatedAt", value.value("updatedAt")},
            {"operationId", operation},
            {"status", status}};
}
} // namespace

class AdminServiceTest final : public QObject
{
    Q_OBJECT
    DatabaseConnection db_;
    std::unique_ptr<AdminRepository> repo_;
    std::unique_ptr<AdminService> service_;
    QString token_;
    QJsonObject call(const QString& action, const QJsonObject& p = {})
    {
        return service_->handle(action, p, token_);
    }
    qint64 scalar(const QString& sql)
    {
        QSqlQuery q(db_.database());
        if (!q.exec(sql) || !q.next())
            return -1;
        return q.value(0).toLongLong();
    }
private slots:
    void init()
    {
        QVERIFY(db_.open(":memory:", true));
        repo_ = std::make_unique<AdminRepository>(db_.database());
        service_ = std::make_unique<AdminService>(repo_.get());
        token_ = data(service_->handle("auth.login", loginData())).value("sessionToken").toString();
        QVERIFY(!token_.isEmpty());
    }
    void cleanup()
    {
        service_.reset();
        repo_.reset();
        db_.close();
    }
    void authenticationAndRevocation()
    {
        for (const auto& action :
             {"dashboard.get", "stations.list", "users.get", "orders.list", "recharges.list",
              "station.create", "user.status", "charger.restart"})
            QCOMPARE(code(service_->handle(action, {}, "forged")), QString("UNAUTHORIZED"));
        QCOMPARE(
            code(service_->handle("auth.login", {{"username", "admin"}, {"password", "wrong"}})),
            QString("INVALID_CREDENTIALS"));
        const auto login = service_->handle("auth.login", loginData());
        QVERIFY(ok(login));
        const auto publicAdmin = QJsonDocument(data(login).value("admin").toObject()).toJson();
        QVERIFY(!publicAdmin.contains("password"));
        QVERIFY(!publicAdmin.contains("salt"));
        QVERIFY(ok(call("auth.logout")));
        QCOMPARE(code(call("dashboard.get")), QString("UNAUTHORIZED"));
        token_ = data(login).value("sessionToken").toString();
        QSqlQuery q(db_.database());
        QVERIFY(q.exec("UPDATE admins SET status='DISABLED' WHERE id=1"));
        QCOMPARE(code(call("dashboard.get")), QString("UNAUTHORIZED"));
        QCOMPARE(code(service_->handle("auth.login", loginData())), QString("INVALID_CREDENTIALS"));
        QVERIFY(q.exec("UPDATE admins SET status='ACTIVE' WHERE id=1"));
        token_ = data(service_->handle("auth.login", loginData())).value("sessionToken").toString();
        QVERIFY(q.exec("UPDATE admins SET password_salt='changed' WHERE id=1"));
        QCOMPARE(code(call("auth.check")), QString("UNAUTHORIZED"));
    }
    void expiryAndLoginThrottle()
    {
        AdminService shortSession(repo_.get(), 20);
        const auto token =
            data(shortSession.handle("auth.login", loginData())).value("sessionToken").toString();
        QTest::qWait(30);
        QCOMPARE(code(shortSession.handle("dashboard.get", {}, token)), QString("UNAUTHORIZED"));
        for (int i = 0; i < 5; ++i)
            QCOMPARE(
                code(service_->handle("auth.login", {{"username", "admin"}, {"password", "bad"}})),
                QString("INVALID_CREDENTIALS"));
        QCOMPARE(code(service_->handle("auth.login", loginData())), QString("RATE_LIMITED"));
    }
    void queriesValidateAndMask()
    {
        for (const auto& entity : {"stations", "chargers", "users", "orders", "recharges"}) {
            const QString list = QString(entity) + ".list";
            QVERIFY(ok(call(list)));
            QCOMPARE(code(call(list, {{"page", 0}})), QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(list, {{"pageSize", 101}})), QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(list, {{"page", 1.5}})), QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(list, {{"sort", "id; DROP TABLE admins"}})),
                     QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(list, {{"status", "wrong"}})), QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(QString(entity) + ".get", {{"id", 1}})),
                     QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(QString(entity) + ".get", {{"id", "9223372036854775808"}})),
                     QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(QString(entity) + ".get", {{"id", "9999999"}})),
                     QString("NOT_FOUND"));
        }
        QCOMPARE(item(call("users.get", {{"id", "1"}})).value("phone").toString(),
                 QString("138****8000"));
        const auto recharge = call("recharges.list");
        QVERIFY(!QJsonDocument(recharge).toJson().contains("13800138000"));
        QCOMPARE(data(recharge).value("total").toInt(), 1);
        QCOMPARE(code(call("users.list", {{"adminId", "1"}})), QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("orders.edit")), QString("INVALID_ARGUMENT"));
        const auto descending = data(call("stations.list", {{"pageSize", 1}, {"sort", "idDesc"}}));
        QCOMPARE(descending.value("items").toArray().size(), 1);
        QVERIFY(descending.value("total").toInt() >= 1);
    }
    void stationWritesConflictAndDurableReplay()
    {
        const auto created = call("station.create", newStation());
        QVERIFY(ok(created));
        const auto station = item(created);
        const auto count = scalar("SELECT COUNT(*) FROM stations");
        QVERIFY(data(call("station.create", newStation())).value("idempotent").toBool());
        QCOMPARE(scalar("SELECT COUNT(*) FROM stations"), count);
        AdminService restarted(repo_.get());
        const auto token =
            data(restarted.handle("auth.login", loginData())).value("sessionToken").toString();
        QVERIFY(data(restarted.handle("station.create", newStation(), token))
                    .value("idempotent")
                    .toBool());
        auto mismatch = newStation();
        mismatch.insert("name", "不同内容");
        QCOMPARE(code(call("station.create", mismatch)), QString("CONFLICT"));
        // A duplicate charger code must roll back the newly inserted station.
        QCOMPARE(code(call("station.create", newStation("create-2", "NEW-2", "NEW-C1"))),
                 QString("CONFLICT"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM stations"), count);
        auto edit = newStation("edit-1");
        edit.remove("code");
        edit.remove("chargers");
        edit.insert("id", station.value("id"));
        edit.insert("expectedUpdatedAt", station.value("updatedAt"));
        edit.insert("name", "已改名");
        const auto updated = call("station.edit", edit);
        QVERIFY(ok(updated));
        QCOMPARE(item(updated).value("name").toString(), QString("已改名"));
        edit.insert("operationId", "edit-stale");
        QCOMPARE(code(call("station.edit", edit)), QString("CONFLICT"));
        auto off = change(item(updated), "off-1", "INACTIVE");
        const auto stopped = call("station.status", off);
        QVERIFY(ok(stopped));
        QVERIFY(ok(call("station.status", change(item(stopped), "on-1", "ACTIVE"))));
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE target_type='ADMIN_COMMAND'"),
                 4);
    }
    void auditFailureRollsBackAndSafeError()
    {
        const auto before = scalar("SELECT COUNT(*) FROM stations");
        QSqlQuery q(db_.database());
        QVERIFY(q.exec("CREATE TRIGGER fail_admin_audit BEFORE INSERT ON operation_logs BEGIN "
                       "SELECT RAISE(ABORT, 'secret path and SQL'); END"));
        const auto result = call("station.create", newStation());
        QVERIFY(!ok(result));
        QVERIFY(!QJsonDocument(result).toJson().contains("secret"));
        QCOMPARE(scalar("SELECT COUNT(*) FROM stations"), before);
        QCOMPARE(scalar("SELECT COUNT(*) FROM chargers WHERE code='NEW-C1'"), qint64(0));
        QVERIFY(q.exec("DROP TRIGGER fail_admin_audit"));
        QVERIFY(ok(call("station.create", newStation())));
    }
    void activeWorkflowBlocksManagement()
    {
        const auto station = item(call("station.create", newStation()));
        QVERIFY(!station.isEmpty());
        const auto chargers = data(call("chargers.list", {{"stationId", station.value("id")}}))
                                  .value("items")
                                  .toArray();
        QCOMPARE(chargers.size(), 1);
        const auto charger = chargers.first().toObject();
        const auto user = item(call("users.get", {{"id", "1"}}));
        ChargingRepository chargingRepo(db_.database());
        BillingService billing;
        ChargingService charging(&chargingRepo, &billing);
        const auto reservation = charging.reserve(1, charger.value("id").toString().toLongLong());
        QVERIFY(reservation.success);
        auto restart =
            change(item(call("chargers.get", {{"id", charger.value("id")}})), "restart-busy", "");
        restart.remove("status");
        QCOMPARE(code(call("charger.restart", restart)), QString("RESOURCE_BUSY"));
        QCOMPARE(code(call("station.status", change(station, "station-busy", "INACTIVE"))),
                 QString("RESOURCE_BUSY"));
        QCOMPARE(code(call("user.status", change(user, "freeze-busy", "FROZEN"))),
                 QString("RESOURCE_BUSY"));
        QVERIFY(charging.startCharging(1, reservation.reservation.id).success);
        QVERIFY(charging.stopCharging(1, reservation.order.id).success);
        QCOMPARE(code(call("user.status", change(user, "freeze-unpaid", "FROZEN"))),
                 QString("RESOURCE_BUSY"));
        const auto order = call("orders.get", {{"id", QString::number(reservation.order.id)}});
        QVERIFY(ok(order));
        QCOMPARE(item(order).value("phone").toString(), QString("138****8000"));
        // Stop releases the charger; the user's pending payment does not own it.
        auto fault =
            call("charger.status", change(item(call("chargers.get", {{"id", charger.value("id")}})),
                                          "fault-1", "FAULT"));
        QVERIFY(ok(fault));
        restart = change(item(fault), "restart-ok", "");
        restart.remove("status");
        const auto recovered = call("charger.restart", restart);
        QVERIFY(ok(recovered));
        QVERIFY(data(recovered).value("simulated").toBool());
        QCOMPARE(item(recovered).value("status").toString(), QString("AVAILABLE"));
        QVERIFY(data(call("charger.restart", restart)).value("idempotent").toBool());
    }
    void freezeAndUnfreeze()
    {
        const auto user = item(call("users.get", {{"id", "1"}}));
        const auto frozen = call("user.status", change(user, "freeze-1", "FROZEN"));
        QVERIFY(ok(frozen));
        QCOMPARE(item(frozen).value("status").toString(), QString("FROZEN"));
        QVERIFY(ok(call("user.status", change(item(frozen), "unfreeze-1", "ACTIVE"))));
    }
    void dashboardAccounting()
    {
        auto result = call("dashboard.get", {{"days", 30}});
        QVERIFY(ok(result));
        QCOMPARE(data(result).value("trend").toArray().size(), 30);
        QCOMPARE(data(result).value("todayRevenueCents").toInteger(), qint64(0));
        QCOMPARE(data(result).value("timeZone").toString(), QString("UTC"));
        QCOMPARE(code(call("dashboard.get", {{"days", 8}})), QString("INVALID_ARGUMENT"));
        // The seed recharge is not counted as revenue. Only a paid order is.
        QSqlQuery q(db_.database());
        QVERIFY(
            q.exec("INSERT INTO "
                   "orders(order_no,user_id,charger_id,status,unit_price_cents_per_kwh,amount_"
                   "cents,started_at,stopped_at,paid_at) "
                   "VALUES('PAID-1',1,1,'COMPLETED',120,432,strftime('%Y-%m-%dT%H:%M:%fZ','now'),"
                   "strftime('%Y-%m-%dT%H:%M:%fZ','now'),strftime('%Y-%m-%dT%H:%M:%fZ','now'))"));
        result = call("dashboard.get");
        QCOMPARE(data(result).value("todayRevenueCents").toInteger(), qint64(432));
        QCOMPARE(data(result).value("monthRevenueCents").toInteger(), qint64(432));
        QCOMPARE(data(result).value("trend").toArray().size(), 7);
    }
    void dashboardSummariesShareManagementData()
    {
        QSqlQuery q(db_.database());
        QVERIFY(q.exec("UPDATE chargers SET status='AVAILABLE'"));
        const auto empty = data(call("dashboard.get"));
        QVERIFY(empty.value("abnormalChargers").toObject().value("items").toArray().isEmpty());
        QCOMPARE(empty.value("abnormalChargers").toObject().value("total").toInt(), 0);
        QVERIFY(empty.value("latestOrders").toObject().value("items").toArray().isEmpty());
        for (int i = 0; i < 7; ++i) {
            q.prepare("INSERT INTO chargers(station_id,code,type,power_watts,status,updated_at) "
                      "VALUES(1,?,'SLOW',7200,?,?)");
            q.addBindValue(QString("ABNORMAL-%1").arg(i));
            q.addBindValue(i % 2 ? "OFFLINE" : "FAULT");
            q.addBindValue(i == 0 ? "2026-09-07T00:00:00.000Z" : "2026-09-05T00:00:00.000Z");
            QVERIFY(q.exec());
            q.prepare("INSERT INTO "
                      "orders(id,order_no,user_id,charger_id,status,unit_price_cents_per_kwh,"
                      "created_at) VALUES(?, ?,1,1,'CANCELLED',120,?)");
            q.addBindValue(100 + i);
            q.addBindValue(QString("LATEST-%1").arg(i));
            q.addBindValue(i == 0 ? "2026-09-08T00:00:00.000Z" : "2026-09-05T00:00:00.000Z");
            QVERIFY(q.exec());
        }
        const QJsonObject abnormalQuery{
            {"abnormalOnly", true}, {"sort", "updatedAtDesc"}, {"pageSize", 5}};
        const QJsonObject ordersQuery{{"sort", "createdAtDesc"}, {"pageSize", 5}};
        auto dashboard = data(call("dashboard.get"));
        const auto abnormalities = dashboard.value("abnormalChargers").toObject();
        const auto latest = dashboard.value("latestOrders").toObject();
        QCOMPARE(abnormalities, data(call("chargers.list", abnormalQuery)));
        QCOMPARE(latest, data(call("orders.list", ordersQuery)));
        QCOMPARE(abnormalities.value("items").toArray().size(), 5);
        QCOMPARE(abnormalities.value("total").toInt(), 7);
        QCOMPARE(dashboard.value("faultChargers").toInt() +
                     dashboard.value("offlineChargers").toInt(),
                 7);
        QCOMPARE(latest.value("items").toArray().size(), 5);
        QCOMPARE(latest.value("total").toInt(), 7);
        QCOMPARE(latest.value("items").toArray().at(0).toObject().value("id").toString(),
                 QString("100"));
        QCOMPARE(latest.value("items").toArray().at(1).toObject().value("id").toString(),
                 QString("106"));
        QVERIFY(!QJsonDocument(dashboard).toJson().contains("13800138000"));
        for (const auto& value : abnormalities.value("items").toArray()) {
            const auto charger = value.toObject();
            QCOMPARE(charger.value("exceptionType"), charger.value("status"));
            QVERIFY(charger.value("exceptionType") == "FAULT" ||
                    charger.value("exceptionType") == "OFFLINE");
            QVERIFY(!charger.contains("exceptionAt")); // no invented event time
            QCOMPARE(charger, item(call("chargers.get", {{"id", charger.value("id")}})));
        }
        auto restart = change(abnormalities.value("items").toArray().first().toObject(),
                              "summary-restart", "");
        restart.remove("status");
        const auto recovered = call("charger.restart", restart);
        QVERIFY(ok(recovered));
        QVERIFY(item(recovered).value("exceptionType").isNull());
        dashboard = data(call("dashboard.get"));
        QCOMPARE(dashboard.value("abnormalChargers").toObject().value("total").toInt(), 6);
        QCOMPARE(dashboard.value("abnormalChargers").toObject(),
                 data(call("chargers.list", abnormalQuery)));
    }
    void orderSemanticFiltersAndValidation()
    {
        const auto station = item(call("station.create", newStation()));
        QVERIFY(!station.isEmpty());
        const auto charger = data(call("chargers.list", {{"stationId", station.value("id")}}))
                                 .value("items")
                                 .toArray()
                                 .first()
                                 .toObject();
        QSqlQuery q(db_.database());
        for (int i = 0; i < 4; ++i) {
            q.prepare("INSERT INTO "
                      "orders(order_no,user_id,charger_id,status,unit_price_cents_per_kwh,created_"
                      "at) VALUES(?,1,?,'CANCELLED',120,?)");
            q.addBindValue(QString("FILTER-%1").arg(i));
            q.addBindValue(i == 2 ? "1" : charger.value("id").toString());
            q.addBindValue(i == 3 ? "2026-09-06T00:00:00.000Z" : "2026-09-05T00:00:00.000Z");
            QVERIFY(q.exec());
        }
        QJsonObject filter{{"stationId", station.value("id")},
                           {"chargerId", charger.value("id")},
                           {"userId", "1"},
                           {"status", "CANCELLED"},
                           {"keyword", "FILTER"},
                           {"createdAtFrom", "2026-09-05T00:00:00.000Z"},
                           {"createdAtTo", "2026-09-06T00:00:00.000Z"},
                           {"sort", "createdAtDesc"},
                           {"pageSize", 1}};
        auto result = data(call("orders.list", filter));
        QCOMPARE(result.value("total").toInt(), 2);
        QCOMPARE(result.value("items").toArray().first().toObject().value("orderNo").toString(),
                 QString("FILTER-1"));
        filter.insert("page", 2);
        result = data(call("orders.list", filter));
        QCOMPARE(result.value("total").toInt(), 2);
        QCOMPARE(result.value("items").toArray().first().toObject().value("orderNo").toString(),
                 QString("FILTER-0"));
        for (const auto& invalid :
             {"2026-02-30T00:00:00.000Z", "2026-09-05", "2026-09-05T00:00:00Z",
              "2026-09-05T00:00:00.000+08:00", "' OR 1=1 --"}) {
            QCOMPARE(code(call("orders.list", {{"createdAtFrom", invalid}})),
                     QString("INVALID_ARGUMENT"));
        }
        QCOMPARE(code(call("orders.list", {{"createdAtFrom", "2026-09-06T00:00:00.000Z"},
                                           {"createdAtTo", "2026-09-06T00:00:00.000Z"}})),
                 QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("orders.list", {{"createdAtFrom", "2026-09-07T00:00:00.000Z"},
                                           {"createdAtTo", "2026-09-06T00:00:00.000Z"}})),
                 QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("orders.list", {{"stationId", 1}})), QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("orders.list", {{"chargerId", "0"}})), QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("orders.list", {{"sort", "createdAtDesc;DELETE"}})),
                 QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("users.list", {{"sort", "createdAtDesc"}})),
                 QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("orders.list", {{"sort", "updatedAtDesc"}})),
                 QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("chargers.list", {{"abnormalOnly", "true"}})),
                 QString("INVALID_ARGUMENT"));
        QCOMPARE(code(call("users.list", {{"abnormalOnly", true}})), QString("INVALID_ARGUMENT"));
    }
    void concurrentRetryCommitsOnce()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("concurrent.sqlite");
        DatabaseConnection setup;
        QVERIFY(setup.open(path, true));
        QJsonObject results[2];
        std::atomic<int> ready{0};
        const auto worker = [&](int index) {
            DatabaseConnection connection;
            if (!connection.open(path, false)) {
                ++ready;
                return;
            }
            AdminRepository repository(connection.database());
            AdminService service(&repository);
            const auto token =
                data(service.handle("auth.login", loginData())).value("sessionToken").toString();
            ++ready;
            while (ready.load() < 2)
                std::this_thread::yield();
            results[index] = service.handle("station.create", newStation(), token);
        };
        std::thread first(worker, 0), second(worker, 1);
        first.join();
        second.join();
        QVERIFY(ok(results[0]));
        QVERIFY(ok(results[1]));
        QVERIFY(data(results[0]).value("idempotent").toBool() !=
                data(results[1]).value("idempotent").toBool());
        QCOMPARE(item(results[0]).value("id"), item(results[1]).value("id"));
        QSqlQuery q(setup.database());
        QVERIFY(q.exec("SELECT COUNT(*) FROM operation_logs WHERE target_type='ADMIN_COMMAND'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
    }
};
QTEST_GUILESS_MAIN(AdminServiceTest)
#include "tst_admin_service.moc"
