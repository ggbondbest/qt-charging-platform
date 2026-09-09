#include "admin_repository.h"
#include "admin_service.h"
#include "database_connection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>
#include <memory>

using namespace charging::server;
namespace {
QJsonObject data(const QJsonObject& r) { return r.value("data").toObject(); }
QJsonObject item(const QJsonObject& r) { return data(r).value("item").toObject(); }
QString code(const QJsonObject& r) { return r.value("error").toObject().value("code").toString(); }
bool ok(const QJsonObject& r) { return r.value("success").toBool(); }
QJsonObject station(const QString& operation = "contacts-create")
{
    return {{"operationId", operation}, {"code", "CONTACTS-1"}, {"name", "联系信息测试站"},
            {"address", "高新区测试路1号"}, {"city", "大连市"}, {"district", "高新区"},
            {"contactName", "测试负责人"}, {"contactPhone", "13900139000"},
            {"latitude", 38.8}, {"longitude", 121.5}, {"priceCentsPerKwh", 123},
            {"chargers", QJsonArray{QJsonObject{{"code", "CONTACTS-C1"}, {"type", "FAST"},
                                                {"powerWatts", 80000}}}}};
}
}

class AdminExtensionQueriesTest final : public QObject
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
        QSqlQuery query(db_.database());
        return query.exec(sql) && query.next() ? query.value(0).toLongLong() : -1;
    }
private slots:
    void init()
    {
        QString error;
        QVERIFY2(db_.open(":memory:", true, &error), qPrintable(error));
        repo_ = std::make_unique<AdminRepository>(db_.database());
        service_ = std::make_unique<AdminService>(repo_.get());
        token_ = data(service_->handle("auth.login", {{"username", "admin"}, {"password", "123456"}}))
                     .value("sessionToken").toString();
        QVERIFY(!token_.isEmpty());
    }
    void cleanup()
    {
        service_.reset();
        repo_.reset();
        db_.close();
    }
    void stationContactsPersistMaskAndReplay()
    {
        const auto command = station();
        const auto created = call("station.create", command);
        QVERIFY2(ok(created), qPrintable(code(created)));
        const auto saved = item(created);
        QCOMPARE(saved.value("city").toString(), QString("大连市"));
        QCOMPARE(saved.value("district").toString(), QString("高新区"));
        QCOMPARE(saved.value("contactName").toString(), QString("测试负责人"));
        QCOMPARE(saved.value("contactPhone").toString(), QString("13900139000"));
        const auto listed = data(call("stations.list", {{"city", "大连市"}, {"district", "高新区"}}));
        QCOMPARE(listed.value("total").toInt(), 1);
        QCOMPARE(listed.value("items").toArray().first().toObject().value("contactPhone").toString(),
                 QString("139****9000"));
        QCOMPARE(data(call("stations.summary", {{"city", "大连市"}, {"district", "高新区"}}))
                     .value("totalStations").toInt(), 1);
        QVERIFY(!QJsonDocument(listed).toJson().contains("13900139000"));
        QCOMPARE(item(call("stations.get", {{"id", saved.value("id")}})), saved);
        QVERIFY(data(call("station.create", command)).value("idempotent").toBool());
        QCOMPARE(scalar("SELECT COUNT(*) FROM operation_logs WHERE action='station.create'"), 1LL);
        auto edit = command;
        edit.remove("code");
        edit.remove("chargers");
        edit.insert("id", saved.value("id"));
        edit.insert("expectedUpdatedAt", saved.value("updatedAt"));
        edit.insert("operationId", "contacts-edit");
        edit.insert("contactPhone", "13700137000");
        const auto edited = call("station.edit", edit);
        QVERIFY(ok(edited));
        QCOMPARE(item(edited).value("contactPhone").toString(), QString("13700137000"));
        auto stale = edit;
        stale.insert("operationId", "contacts-stale");
        QCOMPARE(code(call("station.edit", stale)), QString("CONFLICT"));
        edit.insert("contactPhone", "13800138000");
        QCOMPARE(code(call("station.edit", edit)), QString("CONFLICT"));
        QCOMPARE(item(call("stations.get", {{"id", saved.value("id")}})).value("contactPhone").toString(),
                 QString("13700137000"));
    }
    void legacyContactsAndValidation()
    {
        const auto legacy = item(call("stations.get", {{"id", "1"}}));
        QVERIFY(legacy.value("city").isNull());
        QVERIFY(legacy.value("contactPhone").isNull());
        auto edit = station("legacy-edit");
        edit.remove("code");
        edit.remove("chargers");
        for (const auto& key : {"city", "district", "contactName", "contactPhone"})
            edit.remove(key);
        edit.insert("id", "1");
        edit.insert("expectedUpdatedAt", legacy.value("updatedAt"));
        auto response = call("station.edit", edit);
        QVERIFY(ok(response));
        QVERIFY(item(response).value("contactPhone").isNull());
        const QList<QPair<QString, QJsonValue>> invalid{
            {"city", ""}, {"district", QString(65, QLatin1Char('x'))}, {"contactName", " 负责人"},
            {"contactPhone", "10000000000"}, {"contactPhone", "13900139000x"}, {"city", QJsonValue::Null},
            {"contactPhone", 13900139000.0}};
        for (const auto& field : invalid) {
            auto command = station();
            command.insert(field.first, field.second);
            QCOMPARE(code(call("station.create", command)), QString("INVALID_ARGUMENT"));
        }
        QCOMPARE(scalar("SELECT COUNT(*) FROM stations WHERE code='CONTACTS-1'"), 0LL);
    }
    void stationAuditFailureRollsBackContactsAndChildren()
    {
        QSqlQuery query(db_.database());
        QVERIFY(query.exec("CREATE TRIGGER reject_extension_audit BEFORE INSERT ON operation_logs "
                           "BEGIN SELECT RAISE(ABORT,'audit unavailable'); END"));
        QVERIFY(!ok(call("station.create", station())));
        QCOMPARE(scalar("SELECT COUNT(*) FROM stations WHERE code='CONTACTS-1'"), 0LL);
        QCOMPARE(scalar("SELECT COUNT(*) FROM chargers WHERE code='CONTACTS-C1'"), 0LL);
        QVERIFY(query.exec("DROP TRIGGER reject_extension_audit"));
        QVERIFY(ok(call("station.create", station())));
    }
    void powerRangeAndSummaryUseSameScope()
    {
        const QJsonObject filter{{"minPowerWatts", 60000}, {"maxPowerWatts", 120000},
                                 {"type", "FAST"}};
        const auto listed = data(call("chargers.list", filter));
        QCOMPARE(listed.value("total").toInt(), 3);
        QCOMPARE(data(call("chargers.summary", filter)).value("totalChargers").toInt(), 3);
        QCOMPARE(data(call("chargers.list", {{"powerWatts", 7000}})).value("total").toInt(), 2);
        for (const auto& invalid : {
                 QJsonObject{{"minPowerWatts", 120001}, {"maxPowerWatts", 120000}},
                 QJsonObject{{"minPowerWatts", 0}}, QJsonObject{{"maxPowerWatts", "7000"}},
                 QJsonObject{{"powerWatts", 7000}, {"minPowerWatts", 7000}}}) {
            QCOMPARE(code(call("chargers.list", invalid)), QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call("chargers.summary", invalid)), QString("INVALID_ARGUMENT"));
        }
    }
    void userRangesAreInclusiveMoneyHalfOpenTime()
    {
        QSqlQuery query(db_.database());
        QVERIFY(query.exec("INSERT INTO users(phone,nickname,balance_cents,created_at) VALUES"
                           "('13900139000','截止用户',20000,'2026-09-02T00:00:00.000Z'),"
                           "('13700137000','区间用户',15000,'2026-09-01T12:00:00.000Z')"));
        const QJsonObject filter{{"createdAtFrom", "2026-09-01T00:00:00.000Z"},
                                 {"createdAtTo", "2026-09-02T00:00:00.000Z"},
                                 {"minBalanceCents", 10000}, {"maxBalanceCents", 15000}};
        const auto listed = data(call("users.list", filter));
        QCOMPARE(listed.value("total").toInt(), 2);
        const auto summary = data(call("users.summary", filter));
        QCOMPARE(summary.value("totalUsers").toInt(), 2);
        QCOMPARE(summary.value("totalBalanceCents").toInt(), 25000);
        for (const auto& invalid : {
                 QJsonObject{{"minBalanceCents", -1}}, QJsonObject{{"maxBalanceCents", 1.5}},
                 QJsonObject{{"minBalanceCents", 20}, {"maxBalanceCents", 10}},
                 QJsonObject{{"createdAtFrom", "2026-09-01"}},
                 QJsonObject{{"createdAtFrom", "2026-09-02T00:00:00.000Z"},
                             {"createdAtTo", "2026-09-02T00:00:00.000Z"}}})
            QCOMPARE(code(call("users.list", invalid)), QString("INVALID_ARGUMENT"));
    }
    void orderFiltersCombineAndEscapeWildcards()
    {
        QSqlQuery query(db_.database());
        QVERIFY(query.exec("UPDATE users SET nickname='昵称%测试' WHERE id=1"));
        QVERIFY(query.exec("INSERT INTO users(id,phone,nickname) VALUES(2,'13900139000','昵称测试')"));
        QVERIFY(query.exec("INSERT INTO orders(order_no,user_id,charger_id,status,amount_cents,"
                           "unit_price_cents_per_kwh,started_at,stopped_at,paid_at) VALUES"
                           "('EXACT-ORDER-1',1,1,'COMPLETED',700,120,'2026-09-01T00:00:00.000Z',"
                           "'2026-09-01T01:00:00.000Z','2026-09-01T01:00:00.000Z'),"
                           "('EXACT-ORDER-2',2,1,'COMPLETED',500,120,'2026-09-01T00:00:00.000Z',"
                           "'2026-09-01T01:00:00.000Z','2026-09-01T01:00:00.000Z')"));
        QJsonObject filter{{"orderNo", "EXACT-ORDER-1"}, {"userKeyword", "%测试"},
                           {"phone", "13800138000"}};
        QCOMPARE(data(call("orders.list", filter)).value("total").toInt(), 1);
        QCOMPARE(data(call("orders.summary", filter)).value("totalRevenueCents").toInt(), 700);
        filter.insert("phone", "13900139000");
        QCOMPARE(data(call("orders.list", filter)).value("total").toInt(), 0);
        QCOMPARE(data(call("orders.summary", filter)).value("totalRevenueCents").toInt(), 0);
        QCOMPARE(data(call("orders.list", {{"orderNo", "EXACT-ORDER"}})).value("total").toInt(), 0);
        QCOMPARE(data(call("orders.list", {{"userKeyword", "%"}})).value("total").toInt(), 1);
        QCOMPARE(code(call("orders.list", {{"phone", "138"}})), QString("INVALID_ARGUMENT"));
    }
    void idleIsIndependentOfStationStatus()
    {
        QSqlQuery query(db_.database());
        QVERIFY(query.exec("UPDATE chargers SET status='RESERVED' WHERE id=1"));
        QVERIFY(query.exec("UPDATE stations SET status='INACTIVE' WHERE id=2"));
        QCOMPARE(data(call("stations.list", {{"idleOnly", true}})).value("total").toInt(), 2);
        const QJsonObject filter{{"idleOnly", true}, {"status", "ACTIVE"}};
        QCOMPARE(data(call("stations.list", filter)).value("total").toInt(), 1);
        QCOMPARE(data(call("stations.summary", filter)).value("totalStations").toInt(), 1);
        QCOMPARE(data(call("stations.list", {{"idleOnly", false}})).value("total").toInt(), 3);
        QCOMPARE(code(call("stations.list", {{"idleOnly", "true"}})), QString("INVALID_ARGUMENT"));
    }
    void optionsPaginateBeyondOneHundredAndRemainSafe()
    {
        QSqlQuery query(db_.database());
        QVERIFY(query.exec("BEGIN"));
        for (int i = 1; i <= 105; ++i) {
            query.prepare("INSERT INTO stations(code,name,address,latitude,longitude,price_cents_per_kwh) VALUES(?,?,?,?,?,120)");
            query.addBindValue(QString("OPTION-%1").arg(i, 3, 10, QLatin1Char('0')));
            query.addBindValue(QString("下拉测试站%1").arg(i));
            query.addBindValue("测试路"); query.addBindValue(38.8); query.addBindValue(121.5);
            QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
            const auto stationId = query.lastInsertId();
            query.prepare("INSERT INTO chargers(station_id,code,type,power_watts) VALUES(?,?,'FAST',60000)");
            query.addBindValue(stationId);
            query.addBindValue(QString("OPTION-CHARGER-%1").arg(i, 3, 10, QLatin1Char('0')));
            QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
        }
        QVERIFY(query.exec("COMMIT"));
        const auto first = data(call("stations.options", {{"keyword", "OPTION-"}, {"pageSize", 100}}));
        QCOMPARE(first.value("total").toInt(), 105);
        QCOMPARE(first.value("items").toArray().size(), 100);
        const auto second = data(call("stations.options", {{"keyword", "OPTION-"}, {"pageSize", 100}, {"page", 2}}));
        QCOMPARE(second.value("items").toArray().size(), 5);
        const auto match = data(call("stations.options", {{"keyword", "OPTION-105"}}));
        QCOMPARE(match.value("items").toArray().size(), 1);
        const auto empty = data(call("stations.options", {{"keyword", "OPTION-"}, {"pageSize", 100}, {"page", 3}}));
        QVERIFY(empty.value("items").toArray().isEmpty());
        QCOMPARE(empty.value("total").toInt(), 105);
        const auto chargerPage = data(call("chargers.options", {{"keyword", "OPTION-CHARGER-"},
                                                               {"pageSize", 100}, {"page", 2}}));
        QCOMPARE(chargerPage.value("total").toInt(), 105);
        QCOMPARE(chargerPage.value("items").toArray().size(), 5);
        const auto chargerSearch = data(call("chargers.options", {{"keyword", "OPTION-CHARGER-105"}}));
        QCOMPARE(chargerSearch.value("total").toInt(), 1);
        QCOMPARE(chargerSearch.value("items").toArray().first().toObject().value("code").toString(),
                 QString("OPTION-CHARGER-105"));
        const auto chargers = data(call("chargers.options", {{"stationId", "1"}, {"pageSize", 1}, {"page", 2}}));
        QCOMPARE(chargers.value("total").toInt(), 3);
        const auto charger = chargers.value("items").toArray().first().toObject();
        QCOMPARE(charger.value("stationId").toString(), QString("1"));
        QVERIFY(!charger.value("stationName").toString().isEmpty());
        QCOMPARE(charger.value("code"), charger.value("name"));
        const auto admins = data(call("admins.options"));
        QCOMPARE(admins.value("total").toInt(), 1);
        const auto json = QJsonDocument(admins).toJson();
        QVERIFY(!json.contains("password")); QVERIFY(!json.contains("token"));
        for (const auto& action : {"stations.options", "chargers.options", "admins.options"}) {
            QCOMPARE(code(service_->handle(action, {})), QString("UNAUTHORIZED"));
            QCOMPARE(code(call(action, {{"pageSize", 101}})), QString("INVALID_ARGUMENT"));
            QCOMPARE(code(call(action, {{"unknown", true}})), QString("INVALID_ARGUMENT"));
        }
    }
    void actionMetadataAndTransactionMasking()
    {
        const auto response = call("operation_logs.actions");
        QVERIFY(ok(response));
        QSet<QString> actions;
        for (const auto& value : data(response).value("items").toArray()) {
            const auto metadata = value.toObject();
            QVERIFY(!metadata.value("category").toString().isEmpty());
            QVERIFY(!metadata.value("valueLabel").toString().isEmpty());
            actions.insert(metadata.value("action").toString());
        }
        QVERIFY(actions.contains("station.create"));
        QVERIFY(actions.contains("charger_exceptions.recover"));
        QCOMPARE(actions.size(), 7);
        QCOMPARE(code(call("operation_logs.actions", {{"keyword", ""}})), QString("INVALID_ARGUMENT"));
        const auto recharge = item(call("recharges.get", {{"id", "1"}}));
        QVERIFY(recharge.value("transactionNo").toString().contains("****"));
        QVERIFY(!QJsonDocument(recharge).toJson().contains("SEED-RECHARGE-0001"));
    }
};

QTEST_GUILESS_MAIN(AdminExtensionQueriesTest)
#include "tst_admin_extension_queries.moc"
