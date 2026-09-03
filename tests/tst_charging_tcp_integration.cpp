#include "billing_service.h"
#include "charging/common/protocol/protocol.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "database_connection.h"
#include "network/network_manager.h"
#include "order_repository.h"
#include "order_service.h"
#include "request_dispatcher.h"
#include "tcp_server.h"
#include "user_repository.h"
#include "user_service.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

namespace {

struct RequestOutcome
{
    bool responseReceived = false;
    charging::protocol::ResponseEnvelope response;
    QString requestId;
    QString transportErrorCode;
    QString transportErrorMessage;
};

RequestOutcome sendAndWait(charging::client::network::NetworkManager* connection,
                           const QString& type, const QJsonObject& data)
{
    RequestOutcome outcome;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(5000);

    QObject::connect(connection, &charging::client::network::NetworkManager::responseReceived,
                     &loop, [&](const charging::protocol::ResponseEnvelope& response) {
                         if (response.requestId != outcome.requestId) {
                             return;
                         }
                         outcome.response = response;
                         outcome.responseReceived = true;
                         loop.quit();
                     });
    QObject::connect(
        connection, &charging::client::network::NetworkManager::requestFailed, &loop,
        [&](const QString& requestId, const QString& errorCode, const QString& message) {
            if (requestId != outcome.requestId) {
                return;
            }
            outcome.transportErrorCode = errorCode;
            outcome.transportErrorMessage = message;
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        outcome.transportErrorCode = QStringLiteral("TEST_TIMEOUT");
        outcome.transportErrorMessage = QStringLiteral("Timed out waiting for TCP response");
        loop.quit();
    });

    outcome.requestId = connection->sendRequest(type, data);
    timeout.start();
    loop.exec();
    return outcome;
}

class TcpWorkflowFixture final
{
public:
    bool start(QString* errorMessage)
    {
        if (!directory_.isValid()) {
            *errorMessage = QStringLiteral("Unable to create temporary directory");
            return false;
        }
        if (!database_.open(directory_.filePath(QStringLiteral("tcp-workflow.sqlite3")), false,
                            errorMessage)) {
            return false;
        }

        QSqlQuery query(database_.database());
        if (!query.exec(QStringLiteral("INSERT INTO users (phone, nickname, balance_cents, status) "
                                       "VALUES ('13800000101', 'TCP测试用户', 10000, 'ACTIVE')"))) {
            *errorMessage = query.lastError().text();
            return false;
        }
        userId_ = query.lastInsertId().toLongLong();
        if (!query.exec(
                QStringLiteral("INSERT INTO users (phone, nickname, balance_cents, status) "
                               "VALUES ('13800000102', '伪造目标用户', 10000, 'ACTIVE')"))) {
            *errorMessage = query.lastError().text();
            return false;
        }
        otherUserId_ = query.lastInsertId().toLongLong();
        if (!query.exec(QStringLiteral(
                "INSERT INTO stations "
                "(code, name, address, latitude, longitude, price_cents_per_kwh, status) "
                "VALUES ('STA-TCP-1', 'TCP测试站', '测试地址', 38.9, 121.5, 120, 'ACTIVE')"))) {
            *errorMessage = query.lastError().text();
            return false;
        }
        const qint64 stationId = query.lastInsertId().toLongLong();
        query.prepare(
            QStringLiteral("INSERT INTO chargers "
                           "(station_id, code, type, power_watts, status, "
                           " total_charge_count, total_charge_seconds) "
                           "VALUES (:stationId, 'CHG-TCP-1', 'SLOW', 7200, 'AVAILABLE', 0, 0)"));
        query.bindValue(QStringLiteral(":stationId"), stationId);
        if (!query.exec()) {
            *errorMessage = query.lastError().text();
            return false;
        }
        chargerId_ = query.lastInsertId().toLongLong();

        userRepository_ = std::make_unique<charging::server::UserRepository>(database_.database());
        chargingRepository_ =
            std::make_unique<charging::server::ChargingRepository>(database_.database());
        orderRepository_ =
            std::make_unique<charging::server::OrderRepository>(database_.database());
        userService_ = std::make_unique<charging::server::UserService>(userRepository_.get());
        billingService_ = std::make_unique<charging::server::BillingService>();
        const charging::server::UtcClock clock = [this]() { return nowUtc_; };
        chargingService_ = std::make_unique<charging::server::ChargingService>(
            chargingRepository_.get(), billingService_.get(), clock);
        orderService_ =
            std::make_unique<charging::server::OrderService>(orderRepository_.get(), clock);
        dispatcher_ = std::make_unique<charging::server::RequestDispatcher>(
            userService_.get(), chargingService_.get(), orderService_.get());
        server_ = std::make_unique<charging::server::TcpServer>();
        server_->setRequestDispatcher(dispatcher_.get());
        if (!server_->listen(QHostAddress::LocalHost, 0)) {
            *errorMessage = server_->errorString();
            return false;
        }
        return true;
    }

    void advance(qint64 seconds)
    {
        nowUtc_ = nowUtc_.addSecs(seconds);
    }
    quint16 port() const
    {
        return server_->serverPort();
    }
    qint64 userId() const
    {
        return userId_;
    }
    qint64 otherUserId() const
    {
        return otherUserId_;
    }
    qint64 chargerId() const
    {
        return chargerId_;
    }

    qint64 integer(const QString& sql) const
    {
        QSqlQuery query(database_.database());
        if (!query.exec(sql) || !query.next()) {
            return -1;
        }
        return query.value(0).toLongLong();
    }

    QString text(const QString& sql) const
    {
        QSqlQuery query(database_.database());
        if (!query.exec(sql) || !query.next()) {
            return {};
        }
        return query.value(0).toString();
    }

private:
    QTemporaryDir directory_;
    charging::server::DatabaseConnection database_;
    std::unique_ptr<charging::server::UserRepository> userRepository_;
    std::unique_ptr<charging::server::ChargingRepository> chargingRepository_;
    std::unique_ptr<charging::server::OrderRepository> orderRepository_;
    std::unique_ptr<charging::server::UserService> userService_;
    std::unique_ptr<charging::server::BillingService> billingService_;
    std::unique_ptr<charging::server::ChargingService> chargingService_;
    std::unique_ptr<charging::server::OrderService> orderService_;
    std::unique_ptr<charging::server::RequestDispatcher> dispatcher_;
    std::unique_ptr<charging::server::TcpServer> server_;
    QDateTime nowUtc_ =
        QDateTime::fromString(QStringLiteral("2026-09-03T08:00:00.000Z"), Qt::ISODateWithMs);
    qint64 userId_ = 0;
    qint64 otherUserId_ = 0;
    qint64 chargerId_ = 0;
};

QJsonObject idData(const QString& key, qint64 id, qint64 forgedUserId = 0)
{
    QJsonObject data;
    data.insert(key, QString::number(id));
    if (forgedUserId > 0) {
        data.insert(QStringLiteral("userId"), QString::number(forgedUserId));
    }
    return data;
}

} // namespace

class ChargingTcpIntegrationTest final : public QObject
{
    Q_OBJECT

private slots:
    void sameConnectionCompletesAuthenticatedWorkflow();
};

void ChargingTcpIntegrationTest::sameConnectionCompletesAuthenticatedWorkflow()
{
    using namespace charging::protocol::request_type;
    TcpWorkflowFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::network::NetworkManager client(QStringLiteral("127.0.0.1"), fixture.port());

    const RequestOutcome anonymous = sendAndWait(
        &client, QString::fromLatin1(kReserveCharger),
        idData(QStringLiteral("chargerId"), fixture.chargerId(), fixture.otherUserId()));
    QVERIFY2(anonymous.transportErrorCode.isEmpty(), qPrintable(anonymous.transportErrorMessage));
    QVERIFY(anonymous.responseReceived);
    QVERIFY(!anonymous.response.success);
    QCOMPARE(anonymous.response.error.code,
             QString::fromLatin1(charging::protocol::error_code::kUnauthorized));

    QJsonObject loginData;
    loginData.insert(QStringLiteral("phone"), QStringLiteral("13800000101"));
    const RequestOutcome login = sendAndWait(&client, QString::fromLatin1(kUserLogin), loginData);
    QVERIFY2(login.transportErrorCode.isEmpty(), qPrintable(login.transportErrorMessage));
    QVERIFY(login.responseReceived);
    QVERIFY2(login.response.success, qPrintable(login.response.error.message));
    QCOMPARE(login.response.requestId, login.requestId);
    QCOMPARE(login.response.data.value(QStringLiteral("user"))
                 .toObject()
                 .value(QStringLiteral("id"))
                 .toString(),
             QString::number(fixture.userId()));

    QJsonObject invalidId;
    invalidId.insert(QStringLiteral("chargerId"), static_cast<double>(fixture.chargerId()));
    const RequestOutcome invalid =
        sendAndWait(&client, QString::fromLatin1(kReserveCharger), invalidId);
    QVERIFY(invalid.responseReceived);
    QVERIFY(!invalid.response.success);
    QCOMPARE(invalid.response.type, QString::fromLatin1(kReserveCharger));
    QCOMPARE(invalid.response.requestId, invalid.requestId);
    QCOMPARE(invalid.response.error.code,
             QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope));

    const QStringList invalidStringIds = {QStringLiteral("0"), QStringLiteral("-1"),
                                          QStringLiteral("01")};
    for (const QString& invalidStringId : invalidStringIds) {
        QJsonObject data;
        data.insert(QStringLiteral("chargerId"), invalidStringId);
        const RequestOutcome rejected =
            sendAndWait(&client, QString::fromLatin1(kReserveCharger), data);
        QVERIFY2(rejected.transportErrorCode.isEmpty(), qPrintable(rejected.transportErrorMessage));
        QVERIFY(rejected.responseReceived);
        QVERIFY(!rejected.response.success);
        QCOMPARE(rejected.response.type, QString::fromLatin1(kReserveCharger));
        QCOMPARE(rejected.response.requestId, rejected.requestId);
        QCOMPARE(rejected.response.error.code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope));
    }

    const RequestOutcome reserve = sendAndWait(
        &client, QString::fromLatin1(kReserveCharger),
        idData(QStringLiteral("chargerId"), fixture.chargerId(), fixture.otherUserId()));
    QVERIFY2(reserve.transportErrorCode.isEmpty(), qPrintable(reserve.transportErrorMessage));
    QVERIFY2(reserve.response.success, qPrintable(reserve.response.error.message));
    QCOMPARE(reserve.response.requestId, reserve.requestId);
    const QJsonObject reservation =
        reserve.response.data.value(QStringLiteral("reservation")).toObject();
    const QJsonObject reservedOrder =
        reserve.response.data.value(QStringLiteral("order")).toObject();
    QCOMPARE(reservation.value(QStringLiteral("userId")).toString(),
             QString::number(fixture.userId()));
    QVERIFY(reservation.value(QStringLiteral("userId")).toString() !=
            QString::number(fixture.otherUserId()));
    const qint64 reservationId = reservation.value(QStringLiteral("id")).toString().toLongLong();
    const qint64 orderId = reservedOrder.value(QStringLiteral("id")).toString().toLongLong();
    QVERIFY(reservationId > 0);
    QVERIFY(orderId > 0);

    fixture.advance(1);
    const RequestOutcome start =
        sendAndWait(&client, QString::fromLatin1(kStartCharging),
                    idData(QStringLiteral("reservationId"), reservationId, fixture.otherUserId()));
    QVERIFY2(start.response.success, qPrintable(start.response.error.message));
    QCOMPARE(start.response.type, QString::fromLatin1(kStartCharging));
    QCOMPARE(start.response.requestId, start.requestId);
    QCOMPARE(start.response.data.value(QStringLiteral("order"))
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("CHARGING"));

    charging::client::network::NetworkManager otherClient(QStringLiteral("127.0.0.1"),
                                                          fixture.port());
    QJsonObject otherLoginData;
    otherLoginData.insert(QStringLiteral("phone"), QStringLiteral("13800000102"));
    const RequestOutcome otherLogin =
        sendAndWait(&otherClient, QString::fromLatin1(kUserLogin), otherLoginData);
    QVERIFY2(otherLogin.transportErrorCode.isEmpty(), qPrintable(otherLogin.transportErrorMessage));
    QVERIFY2(otherLogin.response.success, qPrintable(otherLogin.response.error.message));
    QCOMPARE(otherLogin.response.data.value(QStringLiteral("user"))
                 .toObject()
                 .value(QStringLiteral("id"))
                 .toString(),
             QString::number(fixture.otherUserId()));

    const RequestOutcome unauthorizedRead =
        sendAndWait(&otherClient, QString::fromLatin1(kGetChargingStatus),
                    idData(QStringLiteral("orderId"), orderId, fixture.userId()));
    QVERIFY2(unauthorizedRead.transportErrorCode.isEmpty(),
             qPrintable(unauthorizedRead.transportErrorMessage));
    QVERIFY(unauthorizedRead.responseReceived);
    QVERIFY(!unauthorizedRead.response.success);
    QCOMPARE(unauthorizedRead.response.type, QString::fromLatin1(kGetChargingStatus));
    QCOMPARE(unauthorizedRead.response.requestId, unauthorizedRead.requestId);
    QCOMPARE(unauthorizedRead.response.error.code,
             QString::fromLatin1(charging::protocol::error_code::kUnauthorized));

    fixture.advance(250);
    const RequestOutcome live =
        sendAndWait(&client, QString::fromLatin1(kGetChargingStatus),
                    idData(QStringLiteral("orderId"), orderId, fixture.otherUserId()));
    QVERIFY2(live.response.success, qPrintable(live.response.error.message));
    QCOMPARE(live.response.type, QString::fromLatin1(kGetChargingStatus));
    QCOMPARE(live.response.requestId, live.requestId);
    QCOMPARE(live.response.data.value(QStringLiteral("currentPowerWatts")).toInt(), 7200);
    const QJsonObject liveOrder = live.response.data.value(QStringLiteral("order")).toObject();
    QCOMPARE(liveOrder.value(QStringLiteral("durationSeconds")).toInt(), 250);
    QCOMPARE(liveOrder.value(QStringLiteral("energyWh")).toInt(), 500);
    QCOMPARE(liveOrder.value(QStringLiteral("amountCents")).toInt(), 60);

    fixture.advance(250);
    const RequestOutcome stop =
        sendAndWait(&client, QString::fromLatin1(kStopCharging),
                    idData(QStringLiteral("orderId"), orderId, fixture.otherUserId()));
    QVERIFY2(stop.response.success, qPrintable(stop.response.error.message));
    QCOMPARE(stop.response.type, QString::fromLatin1(kStopCharging));
    QCOMPARE(stop.response.requestId, stop.requestId);
    const QJsonObject stoppedOrder = stop.response.data.value(QStringLiteral("order")).toObject();
    QCOMPARE(stoppedOrder.value(QStringLiteral("status")).toString(),
             QStringLiteral("WAITING_PAYMENT"));
    QCOMPARE(stoppedOrder.value(QStringLiteral("amountCents")).toInt(), 120);

    fixture.advance(1);
    const RequestOutcome pay =
        sendAndWait(&client, QString::fromLatin1(kPayOrder),
                    idData(QStringLiteral("orderId"), orderId, fixture.otherUserId()));
    QVERIFY2(pay.response.success, qPrintable(pay.response.error.message));
    QCOMPARE(pay.response.type, QString::fromLatin1(kPayOrder));
    QCOMPARE(pay.response.requestId, pay.requestId);
    QCOMPARE(pay.response.data.value(QStringLiteral("order"))
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("COMPLETED"));
    QCOMPARE(pay.response.data.value(QStringLiteral("balanceCents")).toInt(), 9880);

    QCOMPARE(
        fixture.integer(QStringLiteral("SELECT user_id FROM orders WHERE id = %1").arg(orderId)),
        fixture.userId());
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT balance_cents FROM users WHERE id = %1").arg(fixture.userId())),
        9880);
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("AVAILABLE"));

    // The sixth workflow route, cancellation, is exercised after the first order
    // reaches COMPLETED and therefore no longer blocks a new reservation.
    const RequestOutcome reserveForCancellation =
        sendAndWait(&client, QString::fromLatin1(kReserveCharger),
                    idData(QStringLiteral("chargerId"), fixture.chargerId()));
    QVERIFY2(reserveForCancellation.response.success,
             qPrintable(reserveForCancellation.response.error.message));
    const qint64 cancellationReservationId =
        reserveForCancellation.response.data.value(QStringLiteral("reservation"))
            .toObject()
            .value(QStringLiteral("id"))
            .toString()
            .toLongLong();
    QVERIFY(cancellationReservationId > 0);
    const RequestOutcome cancel =
        sendAndWait(&client, QString::fromLatin1(kCancelReservation),
                    idData(QStringLiteral("reservationId"), cancellationReservationId));
    QVERIFY2(cancel.response.success, qPrintable(cancel.response.error.message));
    QCOMPARE(cancel.response.type, QString::fromLatin1(kCancelReservation));
    QCOMPARE(cancel.response.requestId, cancel.requestId);
    QCOMPARE(cancel.response.data.value(QStringLiteral("reservation"))
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("CANCELLED"));
}

QTEST_GUILESS_MAIN(ChargingTcpIntegrationTest)

#include "tst_charging_tcp_integration.moc"
