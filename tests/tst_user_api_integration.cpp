#include "billing_service.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "charging_server.h"
#include "database_connection.h"
#include "order_repository.h"
#include "order_service.h"
#include "request_dispatcher.h"
#include "user_repository.h"
#include "user_service.h"
#include "user_api_repository.h"
#include "user_api_service.h"
#include "charging/common/model/model_json.h"
#include "charging/client/profile_charging/network_request_transport.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "services/station/station_query_service.h"
#include "services/reservation/reservation_service.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QSettings>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>
#include <future>

using charging::client::network::ClientConnection;
using charging::protocol::ResponseEnvelope;
using namespace charging::protocol::request_type;
namespace {
ResponseEnvelope call(ClientConnection& connection, const QString& type, const QJsonObject& data = {})
{
    ResponseEnvelope result;
    result.error.code = "TEST_TIMEOUT";
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QString id;
    QObject::connect(&connection, &ClientConnection::responseReceived, &loop,
        [&](const ResponseEnvelope& response) { if (response.requestId == id) { result = response; loop.quit(); } });
    QObject::connect(&connection, &ClientConnection::requestFailed, &loop,
        [&](const QString& failedId, const QString& code, const QString& message) {
            if (failedId == id) { result.error.code = code; result.error.message = message; loop.quit(); }
        });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    id = connection.sendRequest(type, data);
    timer.start(4000);
    loop.exec();
    return result;
}
struct Fixture {
    QTemporaryDir dir;
    charging::server::DatabaseConnection db;
    QDateTime now = QDateTime::currentDateTimeUtc();
    std::unique_ptr<charging::server::UserRepository> users;
    std::unique_ptr<charging::server::ChargingRepository> charging;
    std::unique_ptr<charging::server::OrderRepository> orders;
    std::unique_ptr<charging::server::UserApiRepository> api;
    std::unique_ptr<charging::server::UserService> login;
    charging::server::BillingService billing;
    std::unique_ptr<charging::server::ChargingService> chargingService;
    std::unique_ptr<charging::server::OrderService> orderService;
    std::unique_ptr<charging::server::UserApiService> apiService;
    std::unique_ptr<charging::server::RequestDispatcher> dispatcher;
    charging::server::ChargingServer server;
    bool start() {
        if (!db.open(dir.filePath("api.sqlite3"), true)) return false;
        users = std::make_unique<charging::server::UserRepository>(db.database());
        charging = std::make_unique<charging::server::ChargingRepository>(db.database());
        orders = std::make_unique<charging::server::OrderRepository>(db.database());
        api = std::make_unique<charging::server::UserApiRepository>(db.database());
        login = std::make_unique<charging::server::UserService>(users.get());
        const auto clock = [this]() { return now; };
        chargingService = std::make_unique<charging::server::ChargingService>(charging.get(), &billing, clock);
        orderService = std::make_unique<charging::server::OrderService>(orders.get(), clock);
        apiService = std::make_unique<charging::server::UserApiService>(api.get(), clock);
        dispatcher = std::make_unique<charging::server::RequestDispatcher>(login.get(), chargingService.get(), orderService.get(), apiService.get());
        server.setRequestDispatcher(dispatcher.get());
        return server.listen(QHostAddress::LocalHost, 0);
    }
    bool sql(const QString& statement) { QSqlQuery query(db.database()); return query.exec(statement); }
    qint64 number(const QString& statement) {
        QSqlQuery query(db.database());
        return query.exec(statement) && query.next() ? query.value(0).toLongLong() : -1;
    }
};
const QMap<QString, QJsonObject> requests{
    {kGetStations, {}}, {kGetChargers, {{"stationId", "1"}}}, {kGetReservations, {}},
    {kGetUserInfo, {}}, {kUpdateUserInfo, {{"nickname", "小明"}}},
    {kRecharge, {{"amountCents", 500}, {"transactionNo", "auth-1"}}},
    {kGetRechargeRecords, {}}, {kGetOrders, {}},
    {kGetUserStats, {}}, {kGetCoupons, {}}, {kGetNotifications, {}},
    {kCheckIn, {}}, {kGetPoints, {}},
    {kSubmitChargerRating, {{"orderId", "1"}, {"rating", 5}, {"comment", ""}}},
    {kGetMyRatings, {}}
};
} // namespace

class UserApiIntegrationTest final : public QObject {
    Q_OBJECT
private slots:
    void eightRoutesAndWorkflow();
    void statsCouponsAndNotifications();
    void checkInAndPoints();
    void chargerRatings();
    void authorizationAndValidation();
    void pagingExpiryAndLiveLists();
    void rechargeRollbackAndReplay();
    void transportLifecycleAndReconnect();
    void transportTimeout();
    void concurrentDatabaseConnections();
    void persistentRechargeRetry();
};

void UserApiIntegrationTest::eightRoutesAndWorkflow()
{
    Fixture f; QVERIFY(f.start());
    ClientConnection a("127.0.0.1", f.server.serverPort());
    ClientConnection b("127.0.0.1", f.server.serverPort());
    auto response = call(a, kUserLogin, {{"phone", "13900000001"}});
    QVERIFY(response.success);
    const QString uid = response.data.value("user").toObject().value("id").toString();
    QVERIFY(call(b, kUserLogin, {{"phone", "13900000002"}}).success);
    response = call(a, kGetStations);
    QVERIFY2(response.success, qPrintable(response.error.message));
    QCOMPARE(response.data.value("total").toInt(), 3);
    charging::model::Station station;
    QVERIFY(charging::model::fromJson(response.data.value("stations").toArray().first().toObject(), &station));
    QCOMPARE(station.totalChargers, 3);
    QCOMPARE(station.availableChargers, 2);
    response = call(a, kGetChargers, {{"stationId", "1"}, {"pageSize", 2}});
    QVERIFY(response.success);
    QCOMPARE(response.data.value("total").toInt(), 3);
    QCOMPARE(response.data.value("chargers").toArray().size(), 2);
    response = call(a, kReserveCharger, {{"chargerId", "1"}});
    QVERIFY(response.success);
    const QString reservationId = response.data.value("reservation").toObject().value("id").toString();
    const QString orderId = response.data.value("order").toObject().value("id").toString();
    response = call(a, kGetReservations, {{"userId", "1"}});
    QVERIFY(response.success);
    const auto reservation = response.data.value("reservations").toArray().first().toObject();
    QCOMPARE(reservation.value("orderId").toString(), orderId);
    QCOMPARE(reservation.value("userId").toString(), uid);
    QVERIFY(!reservation.value("stationName").toString().isEmpty());
    response = call(b, kGetReservations, {{"userId", uid}});
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 0);
    response = call(b, kGetOrders, {{"userId", uid}});
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 0);
    response = call(a, kUpdateUserInfo, {{"nickname", " 小明 "}, {"avatarKey", "bolt"}, {"balanceCents", 99999}, {"userId", "1"}});
    QVERIFY(response.success);
    QCOMPARE(response.data.value("user").toObject().value("nickname").toString(), QStringLiteral("小明"));
    QCOMPARE(response.data.value("user").toObject().value("balanceCents").toInt(), 0);
    response = call(a, kGetUserInfo);
    QVERIFY(response.success);
    QCOMPARE(response.data.value("user").toObject().value("id").toString(), uid);
    const QJsonObject recharge{{"amountCents", 500}, {"transactionNo", "workflow-recharge"}};
    response = call(a, kRecharge, recharge);
    QVERIFY2(response.success, qPrintable(response.error.message));
    QCOMPARE(response.data.value("balanceCents").toInt(), 500);
    QVERIFY(!response.data.value("idempotent").toBool());
    response = call(a, kGetRechargeRecords);
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 1);
    response = call(b, kGetRechargeRecords, {{"userId", uid}});
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 0);
    QVERIFY(call(a, kStartCharging, {{"reservationId", reservationId}}).success);
    response = call(a, kGetOrders, {{"status", "CHARGING"}});
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 1);
    QCOMPARE(response.data.value("orders").toArray().first().toObject().value("id").toString(), orderId);
    f.now = f.now.addSecs(60);
    QVERIFY(call(a, kStopCharging, {{"orderId", orderId}}).success);
    response = call(a, kPayOrder, {{"orderId", orderId}});
    QVERIFY(response.success);
    const int balance = response.data.value("balanceCents").toInt();
    QVERIFY(balance < 500);
    response = call(a, kRecharge, recharge);
    QVERIFY(response.success); QVERIFY(response.data.value("idempotent").toBool());
    QCOMPARE(response.data.value("balanceCents").toInt(), balance);
    QCOMPARE(response.data.value("record").toObject().value("balanceAfterCents").toInt(), 500);
    QCOMPARE(f.number("SELECT COUNT(*) FROM recharge_records WHERE transaction_no='workflow-recharge'"), 1);
    // A fresh connection to the file sees the committed balance, not mock memory.
    charging::server::DatabaseConnection reopened;
    QVERIFY(reopened.open(f.db.databasePath(), false));
    QSqlQuery query(reopened.database());
    QVERIFY(query.exec("SELECT balance_cents FROM users WHERE id=" + uid));
    QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(), balance);
}

void UserApiIntegrationTest::statsCouponsAndNotifications()
{
    Fixture f; QVERIFY(f.start());
    ClientConnection a("127.0.0.1", f.server.serverPort());
    QVERIFY(call(a, kUserLogin, {{"phone", "13800138000"}}).success);
    QCOMPARE(call(a, kGetNotifications).data.value("total").toInt(), 0);
    QCOMPARE(call(a, kGetCoupons).data.value("total").toInt(), 0);
    QVERIFY(call(a, kGetUserStats).data.value("months").toArray().isEmpty());
    QCOMPARE(call(a, kGetUserStats, {{"months", 13}}).error.code, QStringLiteral("INVALID_ARGUMENT"));

    const auto wf = call(a, kReserveCharger, {{"chargerId", "1"}});
    QVERIFY2(wf.success, qPrintable(wf.error.message));
    const QString reservationId = wf.data.value("reservation").toObject().value("id").toString();
    const QString orderId = wf.data.value("order").toObject().value("id").toString();
    QVERIFY(call(a, kStartCharging, {{"reservationId", reservationId}}).success);
    f.now = f.now.addSecs(600);
    QVERIFY(call(a, kStopCharging, {{"orderId", orderId}}).success);

    auto response = call(a, kGetNotifications);
    QVERIFY2(response.success, qPrintable(response.error.message));
    QCOMPARE(response.data.value("total").toInt(), 1);
    QJsonObject note = response.data.value("notifications").toArray().first().toObject();
    QCOMPARE(note.value("type").toString(), QStringLiteral("charging_stopped"));
    QVERIFY(note.value("body").toString().contains(QStringLiteral("kWh")));
    QVERIFY(note.value("createdAtUtc").isString());
    QVERIFY(!note.contains("userId"));

    QVERIFY(call(a, kPayOrder, {{"orderId", orderId}}).success);
    response = call(a, kGetNotifications, {{"page", 1}, {"pageSize", 1}});
    QVERIFY(response.success);
    QCOMPARE(response.data.value("total").toInt(), 2);
    QCOMPARE(response.data.value("notifications").toArray().first().toObject().value("type").toString(),
             QStringLiteral("order_paid"));   // newest first within one clock tick (id DESC)

    response = call(a, kGetUserStats);
    QVERIFY(response.success);
    const QJsonArray months = response.data.value("months").toArray();
    QCOMPARE(months.size(), 1);
    const QJsonObject month = months.first().toObject();
    QCOMPARE(month.value("orderCount").toInt(), 1);
    QVERIFY(month.value("monthKey").toString().size() == 7);
    QVERIFY(month.value("energyWh").toDouble() > 0);
    QCOMPARE(month.value("co2Grams").toDouble(), qRound64(month.value("energyWh").toDouble() * 0.5568));

    // 批次B 聚合档：同一单数据，year 档键为 4 位年份、week 档为 "YYYY-Www"，
    // 聚合总量必须与 month 档一致（三档只是 GROUP BY 表达式不同）。
    response = call(a, kGetUserStats, {{"period", "year"}});
    QVERIFY(response.success);
    const QJsonArray years = response.data.value("months").toArray();
    QCOMPARE(years.size(), 1);
    const QJsonObject year = years.first().toObject();
    QCOMPARE(year.value("monthKey").toString().size(), 4);
    QCOMPARE(year.value("orderCount").toInt(), 1);
    QCOMPARE(year.value("energyWh").toDouble(), month.value("energyWh").toDouble());
    response = call(a, kGetUserStats, {{"period", "week"}});
    QVERIFY(response.success);
    const QJsonArray weeks = response.data.value("months").toArray();
    QCOMPARE(weeks.size(), 1);
    const QJsonObject week = weeks.first().toObject();
    QVERIFY(QRegularExpression(QStringLiteral("^\\d{4}-W\\d{2}$"))
               .match(week.value("monthKey").toString()).hasMatch());
    QCOMPARE(week.value("amountCents").toDouble(), month.value("amountCents").toDouble());
    QCOMPARE(call(a, kGetUserStats, {{"period", "WEEK"}}).error.code,
             QStringLiteral("INVALID_ARGUMENT"));   // 白名单只收小写

    // Coupon grant: >= ¥50 recharge once; below threshold none; idempotent
    // replay of the same transaction never re-grants.
    QVERIFY(call(a, kRecharge, {{"amountCents", 4999}, {"transactionNo", "coupon-below"}}).success);
    QCOMPARE(call(a, kGetCoupons).data.value("total").toInt(), 0);
    QVERIFY(call(a, kRecharge, {{"amountCents", 5000}, {"transactionNo", "coupon-hit"}}).success);
    response = call(a, kGetCoupons);
    QVERIFY(response.success);
    QCOMPARE(response.data.value("total").toInt(), 1);
    const QJsonObject coupon = response.data.value("coupons").toArray().first().toObject();
    QCOMPARE(coupon.value("kind").toString(), QStringLiteral("cash"));
    QCOMPARE(coupon.value("status").toString(), QStringLiteral("available"));
    QCOMPARE(coupon.value("valueCents").toInt(), 500);
    QCOMPARE(coupon.value("condition").toString(), QStringLiteral("无门槛"));
    QVERIFY(coupon.value("expiresAtUtc").toDouble() > 0);
    QVERIFY(!coupon.value("id").toString().isEmpty());
    QVERIFY(call(a, kRecharge, {{"amountCents", 5000}, {"transactionNo", "coupon-hit"}}).success);
    QCOMPARE(call(a, kGetCoupons).data.value("total").toInt(), 1);
    QCOMPARE(call(a, kGetCoupons, {{"status", "used"}}).data.value("total").toInt(), 0);
    QCOMPARE(call(a, kGetCoupons, {{"status", "AVAILABLE"}}).error.code,
             QStringLiteral("INVALID_ARGUMENT"));
    QCOMPARE(f.number("SELECT COUNT(*) FROM coupons WHERE status='AVAILABLE' AND value_cents=500"), 1);
    // Coupons belong to the session user only; the second account sees none.
    ClientConnection b("127.0.0.1", f.server.serverPort());
    QVERIFY(call(b, kUserLogin, {{"phone", "13900000002"}}).success);
    QCOMPARE(call(b, kGetCoupons).data.value("total").toInt(), 0);
    QCOMPARE(call(b, kGetNotifications).data.value("total").toInt(), 0);
}

void UserApiIntegrationTest::checkInAndPoints()
{
    // 批次C（2026-09-08）：CHECK_IN 日粒度幂等 + GET_POINTS 分页/隔离。
    Fixture f; QVERIFY(f.start());
    ClientConnection a("127.0.0.1", f.server.serverPort());
    QVERIFY(call(a, kUserLogin, {{"phone", "13800138000"}}).success);

    auto response = call(a, kGetPoints);
    QVERIFY2(response.success, qPrintable(response.error.message));
    QCOMPARE(response.data.value("total").toInt(), 0);
    QCOMPARE(response.data.value("points").toInt(), 0);

    response = call(a, kCheckIn);
    QVERIFY(response.success);
    QCOMPARE(response.data.value("gained").toInt(), 10);
    QCOMPARE(response.data.value("alreadyCheckedIn").toBool(), false);
    QCOMPARE(response.data.value("points").toInt(), 10);
    const QString day = response.data.value("day").toString();
    QVERIFY2(QRegularExpression(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$")).match(day).hasMatch(),
             qPrintable(day));
    // 落库对拍：day 与 user_checkins 行一致（响应不是现算的幌子）。
    QCOMPARE(f.number(QStringLiteral(
                 "SELECT COUNT(*) FROM user_checkins WHERE user_id=1 AND day='%1'").arg(day)), 1);

    // 同日重放：幂等成功、不再入账（RECHARGE 同款语义，非错误）。
    response = call(a, kCheckIn);
    QVERIFY(response.success);
    QCOMPARE(response.data.value("alreadyCheckedIn").toBool(), true);
    QCOMPARE(response.data.value("gained").toInt(), 0);
    QCOMPARE(response.data.value("points").toInt(), 10);
    QCOMPARE(f.number("SELECT COUNT(*) FROM points_ledger WHERE user_id=1"), 1);

    // 跨 UTC 日再签两天 → 三条流水，总分 30；分页 pageSize=1 翻页。
    f.now = f.now.addDays(1);
    QVERIFY(call(a, kCheckIn).success);
    f.now = f.now.addDays(1);
    response = call(a, kCheckIn);
    QVERIFY(response.success);
    QCOMPARE(response.data.value("gained").toInt(), 10);
    QCOMPARE(response.data.value("points").toInt(), 30);
    QCOMPARE(f.number("SELECT COUNT(*) FROM user_checkins WHERE user_id=1"), 3);

    response = call(a, kGetPoints, {{"page", 1}, {"pageSize", 1}});
    QVERIFY(response.success);
    QCOMPARE(response.data.value("total").toInt(), 3);
    QCOMPARE(response.data.value("points").toInt(), 30);   // 总分随每页回传
    const QJsonObject entry = response.data.value("entries").toArray().first().toObject();
    QCOMPARE(entry.value("amount").toInt(), 10);
    QCOMPARE(entry.value("reason").toString(), QStringLiteral("每日签到"));  // CHECK_IN 词映射
    QVERIFY(entry.value("createdAtUtc").isString());
    QVERIFY(!entry.contains("userId"));
    QVERIFY(!entry.value("id").toString().isEmpty());
    // 新→旧：第 2 页时间戳必须早于第 1 页。
    const QString newest = entry.value("createdAtUtc").toString();
    response = call(a, kGetPoints, {{"page", 2}, {"pageSize", 1}});
    QCOMPARE(response.data.value("entries").toArray().first().toObject()
                 .value("createdAtUtc").toString().compare(newest) < 0, true);
    QCOMPARE(call(a, kGetPoints, {{"pageSize", 101}}).error.code,
             QStringLiteral("INVALID_ARGUMENT"));

    // 账户隔离：另一账号积分从零开始。
    ClientConnection b("127.0.0.1", f.server.serverPort());
    QVERIFY(call(b, kUserLogin, {{"phone", "13900000002"}}).success);
    QCOMPARE(call(b, kGetPoints).data.value("total").toInt(), 0);
    QCOMPARE(call(b, kCheckIn).data.value("points").toInt(), 10);
}

void UserApiIntegrationTest::chargerRatings()
{
    // 批次E（2026-09-08）：SUBMIT_CHARGER_RATING 一单一评 + GET_MY_RATINGS 分页。
    Fixture f; QVERIFY(f.start());
    ClientConnection a("127.0.0.1", f.server.serverPort());
    QVERIFY(call(a, kUserLogin, {{"phone", "13800138000"}}).success);

    auto response = call(a, kGetMyRatings);
    QVERIFY2(response.success, qPrintable(response.error.message));
    QCOMPARE(response.data.value("total").toInt(), 0);
    QVERIFY(response.data.value("ratings").toArray().isEmpty());

    // 完整工作流造一笔 COMPLETED 单（评价对象绑定订单桩快照）。stats 用例同款
    // 内联形态——lambda 装 QVERIFY 会撞宏内 `return;`（返回值推导冲突）。
    auto wf = call(a, kReserveCharger, {{"chargerId", "1"}});
    QVERIFY2(wf.success, qPrintable(wf.error.message));
    const QString pending = wf.data.value("order").toObject().value("id").toString();
    QVERIFY(call(a, kStartCharging, {{"reservationId",
                 wf.data.value("reservation").toObject().value("id").toString()}}).success);
    f.now = f.now.addSecs(600);
    QVERIFY(call(a, kStopCharging, {{"orderId", pending}}).success);
    // 未支付（WAITING_PAYMENT）不可评价。
    QCOMPARE(call(a, kSubmitChargerRating,
                  {{"orderId", pending}, {"rating", 5}}).error.code,
             QStringLiteral("NOT_FOUND"));
    QVERIFY(call(a, kPayOrder, {{"orderId", pending}}).success);

    response = call(a, kSubmitChargerRating,
                    {{"orderId", pending}, {"rating", 5}, {"comment", "  很快  "}});
    QVERIFY2(response.success, qPrintable(response.error.message));
    QJsonObject row = response.data.value("rating").toObject();
    QCOMPARE(row.value("orderId").toString(), pending);
    QVERIFY(row.value("id").isString());
    QVERIFY(row.value("chargerId").isString());
    QCOMPARE(row.value("rating").toInt(), 5);
    QCOMPARE(row.value("comment").toString(), QStringLiteral("很快"));   // 服务端 trim 落库
    QVERIFY(!row.value("chargerCode").toString().isEmpty());
    QVERIFY(!row.value("stationName").toString().isEmpty());
    QVERIFY(row.value("createdAtUtc").isString());
    QVERIFY(!row.contains("userId"));
    QCOMPARE(response.data.value("alreadyRated").toBool(), false);
    // 落库对拍 + 桩取订单快照（不信客户端）。
    QCOMPARE(f.number(QStringLiteral("SELECT COUNT(*) FROM charger_ratings")), 1);
    QCOMPARE(f.number(QStringLiteral(
                 "SELECT charger_id FROM charger_ratings WHERE order_id=%1").arg(pending)),
             f.number(QStringLiteral("SELECT charger_id FROM orders WHERE id=%1").arg(pending)));

    // 重放：幂等成功、alreadyRated=true、返回首评原值，不产生第二行不改值。
    response = call(a, kSubmitChargerRating,
                    {{"orderId", pending}, {"rating", 1}, {"comment", "改了"}});
    QVERIFY(response.success);
    QCOMPARE(response.data.value("alreadyRated").toBool(), true);
    QCOMPARE(response.data.value("rating").toObject().value("rating").toInt(), 5);
    QCOMPARE(f.number("SELECT COUNT(*) FROM charger_ratings"), 1);
    QCOMPARE(call(a, kGetMyRatings).data.value("ratings").toArray()
                 .first().toObject().value("comment").toString(), QStringLiteral("很快"));

    // 非法入参（normalize 域）。
    QCOMPARE(call(a, kSubmitChargerRating,
                  {{"orderId", pending}, {"rating", 6}}).error.code,
             QStringLiteral("INVALID_ARGUMENT"));
    QCOMPARE(call(a, kSubmitChargerRating,
                  {{"orderId", "0"}, {"rating", 5}}).error.code,
             QStringLiteral("INVALID_ARGUMENT"));

    // 第二单一评 + 分页：新→旧（同 tick 由 id DESC 定序），行形无 userId。
    wf = call(a, kReserveCharger, {{"chargerId", "2"}});
    QVERIFY2(wf.success, qPrintable(wf.error.message));
    const QString second = wf.data.value("order").toObject().value("id").toString();
    QVERIFY(call(a, kStartCharging, {{"reservationId",
                 wf.data.value("reservation").toObject().value("id").toString()}}).success);
    f.now = f.now.addSecs(600);
    QVERIFY(call(a, kStopCharging, {{"orderId", second}}).success);
    QVERIFY(call(a, kPayOrder, {{"orderId", second}}).success);
    QVERIFY(call(a, kSubmitChargerRating,
                 {{"orderId", second}, {"rating", 3}}).success);
    response = call(a, kGetMyRatings, {{"page", 1}, {"pageSize", 1}});
    QVERIFY(response.success);
    QCOMPARE(response.data.value("total").toInt(), 2);
    QCOMPARE(response.data.value("page").toInt(), 1);
    row = response.data.value("ratings").toArray().first().toObject();
    QCOMPARE(row.value("orderId").toString(), second);   // 新单在前
    QVERIFY(!row.value("id").toString().isEmpty());
    QVERIFY(!row.contains("userId"));
    response = call(a, kGetMyRatings, {{"page", 2}, {"pageSize", 1}});
    QCOMPARE(response.data.value("ratings").toArray().first().toObject()
                 .value("orderId").toString(), pending);
    QCOMPARE(call(a, kGetMyRatings, {{"pageSize", 101}}).error.code,
             QStringLiteral("INVALID_ARGUMENT"));

    // 越权：他人订单不可评（NOT_FOUND 与不存在同码，不泄露订单存在性）；列表隔离。
    ClientConnection b("127.0.0.1", f.server.serverPort());
    QVERIFY(call(b, kUserLogin, {{"phone", "13900000002"}}).success);
    QCOMPARE(call(b, kSubmitChargerRating,
                  {{"orderId", pending}, {"rating", 5}}).error.code,
             QStringLiteral("NOT_FOUND"));
    QCOMPARE(call(b, kGetMyRatings).data.value("total").toInt(), 0);
}

void UserApiIntegrationTest::authorizationAndValidation()
{
    Fixture f; QVERIFY(f.start());
    ClientConnection c("127.0.0.1", f.server.serverPort());
    for (auto it = requests.begin(); it != requests.end(); ++it)
        QCOMPARE(call(c, it.key(), it.value()).error.code, QStringLiteral("UNAUTHORIZED"));
    QVERIFY(call(c, kUserLogin, {{"phone", "13800138000"}}).success);
    for (const auto& invalid : QList<QPair<QString,QJsonObject>>{
        {kGetOrders, {{"status", "ACTIVE"}}}, {kGetOrders, {{"page", 0}}},
        {kGetChargers, {{"stationId", 1}}}, {kGetStations, {{"pageSize", 101}}},
        {kUpdateUserInfo, {{"nickname", "valid"}, {"avatarKey", "unknown"}}},
        {kRecharge, {{"amountCents", 100}, {"transactionNo", "bad\n"}}}})
        QCOMPARE(call(c, invalid.first, invalid.second).error.code, QStringLiteral("INVALID_ARGUMENT"));
    QCOMPARE(call(c, kGetUserInfo).data.value("user").toObject().value("nickname").toString(), QStringLiteral("用户8000"));
    QVERIFY(f.sql("UPDATE stations SET status='INACTIVE' WHERE id=3"));
    QCOMPARE(call(c, kGetChargers, {{"stationId", "3"}}).error.code, QStringLiteral("NOT_FOUND"));
    QVERIFY(f.sql("UPDATE users SET status='FROZEN' WHERE id=1"));
    for (auto it = requests.begin(); it != requests.end(); ++it)
        QCOMPARE(call(c, it.key(), it.value()).error.code, QStringLiteral("USER_FROZEN"));
    QCOMPARE(f.number("SELECT balance_cents FROM users WHERE id=1"), 10000);
    // A failed login cannot retain the old authenticated Session.
    QVERIFY(!call(c, kUserLogin, {{"phone", "invalid"}}).success);
    QCOMPARE(call(c, kGetUserInfo).error.code, QStringLiteral("UNAUTHORIZED"));
}

void UserApiIntegrationTest::pagingExpiryAndLiveLists()
{
    Fixture f; QVERIFY(f.start());
    ClientConnection c("127.0.0.1", f.server.serverPort());
    QVERIFY(call(c, kUserLogin, {{"phone", "13800138000"}}).success);
    for (int i = 0; i < 102; ++i) {
        QVERIFY(f.sql(QString("INSERT INTO stations(code,name,address,latitude,longitude,price_cents_per_kwh) VALUES('extra%1','100%%1','literal_',0,0,100)").arg(i)));
    }
    auto response = call(c, kGetStations, {{"keyword", "%"}});
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 102);
    response = call(c, kGetStations, {{"keyword", "' OR 1=1 --"}});
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 0);
    response = call(c, kGetStations, {{"page", 2147483647}, {"pageSize", 100}});
    QVERIFY(response.success); QVERIFY(response.data.value("stations").toArray().isEmpty());
    QCOMPARE(response.data.value("total").toInt(), 105);
    charging::client::services::station::StationQueryService stationService;
    stationService.setConnection(&c); stationService.setLiveMode(true);
    QSignalSpy stations(&stationService, &charging::client::services::station::StationQueryService::querySucceeded);
    stationService.search();
    QTRY_COMPARE_WITH_TIMEOUT(stations.count(), 1, 4000);
    QCOMPARE(qvariant_cast<charging::client::services::station::StationList>(stations.first().at(0)).size(), 105);
    response = call(c, kReserveCharger, {{"chargerId", "1"}});
    QVERIFY(response.success);
    f.now = f.now.addSecs(901);
    response = call(c, kGetReservations, {{"status", "EXPIRED"}});
    QVERIFY(response.success); QCOMPARE(response.data.value("total").toInt(), 1);
    QCOMPARE(f.number("SELECT COUNT(*) FROM orders WHERE status='CANCELLED' AND user_id=1"), 1);
    QCOMPARE(f.number("SELECT COUNT(*) FROM chargers WHERE id=1 AND status='AVAILABLE'"), 1);
    // 批次D（2026-09-08）：超时清扫与翻转同事务落一条通知，词表复用
    // reservation_expiry_reminder（客户端 typeFromServerWord 现成映射）。
    QCOMPARE(f.number("SELECT COUNT(*) FROM notifications "
                      "WHERE type='RESERVATION_EXPIRY_REMINDER' AND user_id=1"), 1);
    response = call(c, kGetNotifications);
    QVERIFY(response.success);
    QCOMPARE(response.data.value("total").toInt(), 1);
    const QJsonObject expiryNote =
        response.data.value("notifications").toArray().first().toObject();
    QCOMPARE(expiryNote.value("type").toString(),
             QStringLiteral("reservation_expiry_reminder"));
    QVERIFY(expiryNote.value("title").toString().contains(QStringLiteral("预约")));
    QVERIFY(!expiryNote.value("body").toString().isEmpty());
    QVERIFY(expiryNote.value("createdAtUtc").isString());
    // 重放清扫（任意读动作再触发）不双写：翻转 UPDATE 以 status='ACTIVE' 守卫。
    QVERIFY(call(c, kGetReservations).success);
    QCOMPARE(call(c, kGetNotifications).data.value("total").toInt(), 1);
    charging::client::services::reservation::ReservationService reservationService;
    reservationService.setConnection(&c); reservationService.setLiveMode(true); reservationService.setUserId(1);
    QSignalSpy reservations(&reservationService, &charging::client::services::reservation::ReservationService::listSucceeded);
    reservationService.fetchList();
    QTRY_COMPARE_WITH_TIMEOUT(reservations.count(), 1, 4000);
    const auto records = qvariant_cast<charging::client::services::reservation::ReservationList>(reservations.first().at(0));
    QCOMPARE(records.size(), 1); QVERIFY(records.first().orderId > 0); QVERIFY(!records.first().stationName.isEmpty());
}

void UserApiIntegrationTest::rechargeRollbackAndReplay()
{
    Fixture f; QVERIFY(f.start());
    ClientConnection a("127.0.0.1", f.server.serverPort()), b("127.0.0.1", f.server.serverPort());
    QVERIFY(call(a, kUserLogin, {{"phone", "13800138000"}}).success);
    QVERIFY(call(b, kUserLogin, {{"phone", "13800138000"}}).success);
    QVERIFY(f.sql("INSERT INTO recharge_records(transaction_no,user_id,amount_cents,balance_after_cents,status) VALUES('failed',1,100,10000,'FAILED')"));
    QCOMPARE(call(a, kRecharge, {{"transactionNo", "failed"}, {"amountCents", 100}}).error.code, QStringLiteral("RECHARGE_FAILED"));
    const QJsonObject data{{"transactionNo", "parallel"}, {"amountCents", 100}};
    QSignalSpy ra(&a, &ClientConnection::responseReceived), rb(&b, &ClientConnection::responseReceived);
    a.sendRequest(kRecharge, data); b.sendRequest(kRecharge, data);
    QTRY_COMPARE_WITH_TIMEOUT(ra.count(), 1, 4000); QTRY_COMPARE_WITH_TIMEOUT(rb.count(), 1, 4000);
    QVERIFY(qvariant_cast<ResponseEnvelope>(ra.first().at(0)).success);
    QVERIFY(qvariant_cast<ResponseEnvelope>(rb.first().at(0)).success);
    QCOMPARE(f.number("SELECT balance_cents FROM users WHERE id=1"), 10100);
    QCOMPARE(f.number("SELECT COUNT(*) FROM recharge_records WHERE transaction_no='parallel'"), 1);
    QCOMPARE(call(a, kRecharge, {{"transactionNo", "parallel"}, {"amountCents", 200}}).error.code, QStringLiteral("IDEMPOTENCY_CONFLICT"));
    QVERIFY(call(b, kUserLogin, {{"phone", "13900000002"}}).success);
    QCOMPARE(call(b, kRecharge, data).error.code, QStringLiteral("IDEMPOTENCY_CONFLICT"));
    QVERIFY(f.sql("CREATE TRIGGER fail_recharge BEFORE INSERT ON recharge_records WHEN NEW.transaction_no='rollback' BEGIN SELECT RAISE(ABORT,'private sql path'); END"));
    auto failed = call(a, kRecharge, {{"transactionNo", "rollback"}, {"amountCents", 100}});
    QCOMPARE(failed.error.code, QStringLiteral("DATABASE_ERROR"));
    QVERIFY(!failed.error.message.contains("private")); QVERIFY(failed.data.isEmpty());
    QCOMPARE(f.number("SELECT balance_cents FROM users WHERE id=1"), 10100);
    QCOMPARE(f.number("SELECT COUNT(*) FROM recharge_records WHERE transaction_no='rollback'"), 0);
    QVERIFY(f.sql("UPDATE users SET balance_cents=9007199254740991 WHERE id=1"));
    QCOMPARE(call(a, kRecharge, {{"transactionNo", "overflow"}, {"amountCents", 1}}).error.code, QStringLiteral("INVALID_ARGUMENT"));
    QCOMPARE(f.number("SELECT COUNT(*) FROM recharge_records WHERE transaction_no='overflow'"), 0);
}

void UserApiIntegrationTest::transportLifecycleAndReconnect()
{
    Fixture f; QVERIFY(f.start());
    ClientConnection c("127.0.0.1", f.server.serverPort());
    QVERIFY(call(c, kUserLogin, {{"phone", "13800138000"}}).success);
    auto transport = std::make_unique<charging::client::NetworkRequestTransport>(&c, 1);
    int callbacks = 0;
    transport->send(kGetUserInfo, {}, [&](bool ok, const QJsonObject& data, const charging::protocol::ProtocolError&) {
        QVERIFY(ok); QCOMPARE(data.value("user").toObject().value("id").toString(), QStringLiteral("1")); ++callbacks;
    });
    QCOMPARE(callbacks, 0);
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 4000);
    transport->send(kGetOrders, {}, [&](bool ok, const QJsonObject&, const charging::protocol::ProtocolError&) {
        QVERIFY(!ok); ++callbacks;
    });
    c.disconnectFromServer();
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 2, 4000);
    QCOMPARE(call(c, kGetUserInfo).error.code, QStringLiteral("UNAUTHORIZED"));
    transport->send(kGetUserInfo, {}, [&](bool ok, const QJsonObject&, const charging::protocol::ProtocolError& error) {
        QVERIFY(!ok); QCOMPARE(error.code, QStringLiteral("UNAUTHORIZED")); ++callbacks;
    });
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 3, 4000);
    QVERIFY(call(c, kUserLogin, {{"phone", "13800138000"}}).success);
    auto* receiver = new QObject;
    transport->sendFor(receiver, kGetUserInfo, {}, [&](bool, const QJsonObject&, const charging::protocol::ProtocolError&) { ++callbacks; });
    delete receiver;
    QTest::qWait(30); QCOMPARE(callbacks, 3);
    transport->send(kGetUserInfo, {}, [&](bool ok, const QJsonObject&, const charging::protocol::ProtocolError&) { QVERIFY(!ok); ++callbacks; });
    transport.reset();
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 4, 4000);
    QTest::qWait(30); QCOMPARE(callbacks, 4);
}

void UserApiIntegrationTest::persistentRechargeRetry()
{
    Fixture f; QVERIFY(f.start());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, f.dir.path());
    ClientConnection c("127.0.0.1", f.server.serverPort());
    QVERIFY(call(c, kUserLogin, {{"phone", "13800138000"}}).success);
    charging::client::NetworkRequestTransport transport(&c, 1);
    const QString key = "pendingRecharge/" + transport.persistenceScope();
    const QJsonObject original{{"amountCents", 500}, {"transactionNo", "persisted-unknown"}};
    // Simulate a previously committed recharge whose success reply was lost.
    QVERIFY(call(c, kRecharge, original).success);
    { QSettings settings; settings.setValue(key, QJsonDocument(original).toJson(QJsonDocument::Compact)); settings.sync(); }
    charging::client::WalletService wallet(&transport);
    QSignalSpy done(&wallet, &charging::client::WalletService::rechargeCompleted);
    QSignalSpy failed(&wallet, &charging::client::WalletService::operationFailed);
    wallet.recharge(600); QCOMPARE(failed.count(), 1);
    wallet.recharge(500);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 4000);
    QCOMPARE(done.first().at(1).toLongLong(), 10500);
    QCOMPARE(f.number("SELECT balance_cents FROM users WHERE id=1"), 10500);
    QCOMPARE(f.number("SELECT COUNT(*) FROM recharge_records WHERE transaction_no='persisted-unknown'"), 1);
    QVERIFY(!QSettings().contains(key));
}

void UserApiIntegrationTest::transportTimeout()
{
    // Transport-only peer: accepts frames but deliberately sends no responses.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
    ClientConnection c("127.0.0.1", silent.serverPort());
    c.sendRequest(kUserLogin, {{"phone", "13800138000"}});
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 2000);
    charging::client::NetworkRequestTransport transport(&c, 1);
    int callbacks = 0;
    transport.send(kGetUserInfo, {}, [&](bool ok, const QJsonObject&, const charging::protocol::ProtocolError& error) {
        QVERIFY(!ok); QCOMPARE(error.code, QStringLiteral("REQUEST_TIMEOUT")); ++callbacks;
    });
    QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 12000);
    c.disconnectFromServer();
    QTest::qWait(30); QCOMPARE(callbacks, 1);
}

void UserApiIntegrationTest::concurrentDatabaseConnections()
{
    Fixture f; QVERIFY(f.start());
    const QString path = f.db.databasePath();
    const auto write = [path]() {
        charging::server::DatabaseConnection db;
        if (!db.open(path, false)) return charging::server::UserApiResult{};
        charging::server::UserApiRepository repository(db.database());
        charging::server::UserApiQuery input;
        input.action = charging::server::UserApiAction::Recharge;
        input.userId = 1; input.transactionNo = "database-race"; input.amountCents = 123;
        input.nowUtc = QDateTime::currentDateTimeUtc();
        return repository.execute(input);
    };
    auto first = std::async(std::launch::async, write);
    auto second = std::async(std::launch::async, write);
    const auto a = first.get(); const auto b = second.get();
    QCOMPARE(a.error, charging::server::UserApiError::None);
    QCOMPARE(b.error, charging::server::UserApiError::None);
    QVERIFY(a.idempotent != b.idempotent);
    QCOMPARE(f.number("SELECT balance_cents FROM users WHERE id=1"), 10123);
    QCOMPARE(f.number("SELECT COUNT(*) FROM recharge_records WHERE transaction_no='database-race'"), 1);
}

QTEST_GUILESS_MAIN(UserApiIntegrationTest)
#include "tst_user_api_integration.moc"
