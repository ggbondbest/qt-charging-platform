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
    {kGetRechargeRecords, {}}, {kGetOrders, {}}
};
} // namespace

class UserApiIntegrationTest final : public QObject {
    Q_OBJECT
private slots:
    void eightRoutesAndWorkflow();
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
