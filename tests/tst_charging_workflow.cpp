#include "billing_service.h"
#include "charging/common/model/enums.h"
#include "charging/common/protocol/protocol.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "database_connection.h"
#include "order_repository.h"
#include "order_service.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

class WorkflowFixture final
{
public:
    bool start(qint64 initialBalanceCents = 10000, qint64 priceCentsPerKwh = 120,
               QString* errorMessage = nullptr)
    {
        if (!directory_.isValid()) {
            return fail(errorMessage, QStringLiteral("Unable to create a temporary directory"));
        }

        QString databaseError;
        if (!database_.open(directory_.filePath(QStringLiteral("workflow.sqlite3")), false,
                            &databaseError)) {
            return fail(errorMessage, databaseError);
        }

        QSqlQuery query(database_.database());
        query.prepare(QStringLiteral("INSERT INTO users (phone, nickname, balance_cents, status) "
                                     "VALUES ('13800000001', '闭环测试用户', :balance, 'ACTIVE')"));
        query.bindValue(QStringLiteral(":balance"), initialBalanceCents);
        if (!query.exec()) {
            return fail(errorMessage, query.lastError().text());
        }
        userId_ = query.lastInsertId().toLongLong();

        if (!query.exec(
                QStringLiteral("INSERT INTO users (phone, nickname, balance_cents, status) "
                               "VALUES ('13800000002', '其他测试用户', 10000, 'ACTIVE')"))) {
            return fail(errorMessage, query.lastError().text());
        }
        otherUserId_ = query.lastInsertId().toLongLong();

        query.prepare(QStringLiteral(
            "INSERT INTO stations "
            "(code, name, address, latitude, longitude, price_cents_per_kwh, status) "
            "VALUES ('STA-TEST-1', '测试站', '测试地址', 38.9, 121.5, :price, 'ACTIVE')"));
        query.bindValue(QStringLiteral(":price"), priceCentsPerKwh);
        if (!query.exec()) {
            return fail(errorMessage, query.lastError().text());
        }
        stationId_ = query.lastInsertId().toLongLong();

        if (!insertCharger(&query, QStringLiteral("CHG-AVAILABLE"), QStringLiteral("AVAILABLE"),
                           &chargerId_, errorMessage) ||
            !insertCharger(&query, QStringLiteral("CHG-FAULT"), QStringLiteral("FAULT"),
                           &faultChargerId_, errorMessage) ||
            !insertCharger(&query, QStringLiteral("CHG-OFFLINE"), QStringLiteral("OFFLINE"),
                           &offlineChargerId_, errorMessage)) {
            return false;
        }

        chargingRepository_ =
            std::make_unique<charging::server::ChargingRepository>(database_.database());
        orderRepository_ =
            std::make_unique<charging::server::OrderRepository>(database_.database());
        billingService_ = std::make_unique<charging::server::BillingService>();
        const charging::server::UtcClock clock = [this]() { return nowUtc_; };
        chargingService_ = std::make_unique<charging::server::ChargingService>(
            chargingRepository_.get(), billingService_.get(), clock);
        orderService_ =
            std::make_unique<charging::server::OrderService>(orderRepository_.get(), clock);
        return true;
    }

    void advance(qint64 seconds)
    {
        nowUtc_ = nowUtc_.addSecs(seconds);
    }

    qint64 integer(const QString& sql, bool* ok = nullptr) const
    {
        QSqlQuery query(database_.database());
        const bool queryOk = query.exec(sql) && query.next();
        bool conversionOk = false;
        const qint64 value = queryOk ? query.value(0).toLongLong(&conversionOk) : 0;
        if (ok != nullptr) {
            *ok = queryOk && conversionOk;
        }
        return value;
    }

    QString text(const QString& sql, bool* ok = nullptr) const
    {
        QSqlQuery query(database_.database());
        const bool queryOk = query.exec(sql) && query.next();
        if (ok != nullptr) {
            *ok = queryOk;
        }
        return queryOk ? query.value(0).toString() : QString();
    }

    QSqlDatabase sqlDatabase() const
    {
        return database_.database();
    }

    charging::server::ChargingService* chargingService() const
    {
        return chargingService_.get();
    }

    charging::server::OrderService* orderService() const
    {
        return orderService_.get();
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
    qint64 faultChargerId() const
    {
        return faultChargerId_;
    }
    qint64 offlineChargerId() const
    {
        return offlineChargerId_;
    }

private:
    static bool fail(QString* errorMessage, const QString& message)
    {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    }

    bool insertCharger(QSqlQuery* query, const QString& code, const QString& status,
                       qint64* chargerId, QString* errorMessage)
    {
        query->prepare(QStringLiteral("INSERT INTO chargers "
                                      "(station_id, code, type, power_watts, status, "
                                      " total_charge_count, total_charge_seconds) "
                                      "VALUES (:stationId, :code, 'SLOW', 7200, :status, 7, 100)"));
        query->bindValue(QStringLiteral(":stationId"), stationId_);
        query->bindValue(QStringLiteral(":code"), code);
        query->bindValue(QStringLiteral(":status"), status);
        if (!query->exec()) {
            return fail(errorMessage, query->lastError().text());
        }
        *chargerId = query->lastInsertId().toLongLong();
        return *chargerId > 0;
    }

    QTemporaryDir directory_;
    charging::server::DatabaseConnection database_;
    std::unique_ptr<charging::server::ChargingRepository> chargingRepository_;
    std::unique_ptr<charging::server::OrderRepository> orderRepository_;
    std::unique_ptr<charging::server::BillingService> billingService_;
    std::unique_ptr<charging::server::ChargingService> chargingService_;
    std::unique_ptr<charging::server::OrderService> orderService_;
    QDateTime nowUtc_ =
        QDateTime::fromString(QStringLiteral("2026-09-03T00:00:00.000Z"), Qt::ISODateWithMs);
    qint64 userId_ = 0;
    qint64 otherUserId_ = 0;
    qint64 stationId_ = 0;
    qint64 chargerId_ = 0;
    qint64 faultChargerId_ = 0;
    qint64 offlineChargerId_ = 0;
};

QString code(const char* value)
{
    return QString::fromLatin1(value);
}

} // namespace

class ChargingWorkflowTest final : public QObject
{
    Q_OBJECT

private slots:
    void completesWorkflowAndPersistsEachState();
    void repeatedStopAndPaymentAreIdempotent();
    void cancellationUpdatesAllThreeRecordsAndIsIdempotent();
    void reservationExpiresAtTheFifteenMinuteBoundary();
    void reservationFreezesPriceAndUnfinishedOrderBlocksAnotherReservation();
    void rejectsUnavailableChargers();
    void rejectsNonOwnerAndIllegalTransitions();
    void insufficientBalanceLeavesOrderUnpaid();
    void rejectsClockRollbackAndInconsistentActiveState();
    void reserveStartAndPaymentFailuresRollBack();
    void failedStopRollsBackEveryTable();
};

void ChargingWorkflowTest::completesWorkflowAndPersistsEachState()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const charging::server::ChargingOperationResult reserved =
        fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY2(reserved.success, qPrintable(reserved.error.message));
    QVERIFY(reserved.reservation.status == charging::model::ReservationStatus::Active);
    QVERIFY(reserved.order.status == charging::model::OrderStatus::Reserved);
    QVERIFY(reserved.charger.status == charging::model::ChargerStatus::Reserved);
    QCOMPARE(reserved.reservation.reservedAtUtc.secsTo(reserved.reservation.expiresAtUtc),
             charging::server::ChargingService::kReservationLifetimeSeconds);
    QCOMPARE(fixture.text(QStringLiteral("SELECT status FROM reservations WHERE id = %1")
                              .arg(reserved.reservation.id)),
             QStringLiteral("ACTIVE"));

    fixture.advance(100);
    const charging::server::ChargingOperationResult started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY2(started.success, qPrintable(started.error.message));
    QVERIFY(started.reservation.status == charging::model::ReservationStatus::Fulfilled);
    QVERIFY(started.order.status == charging::model::OrderStatus::Charging);
    QVERIFY(started.charger.status == charging::model::ChargerStatus::Charging);

    fixture.advance(250);
    const charging::server::ChargingOperationResult live =
        fixture.chargingService()->chargingStatus(fixture.userId(), started.order.id);
    QVERIFY2(live.success, qPrintable(live.error.message));
    QCOMPARE(live.currentPowerWatts, 7200);
    QCOMPARE(live.order.durationSeconds, 250);
    QCOMPARE(live.order.energyWh, 500);
    QCOMPARE(live.order.amountCents, 60);
    // Live metering is a derived snapshot. It must not finalize the database row.
    QCOMPARE(fixture.integer(QStringLiteral("SELECT duration_seconds FROM orders WHERE id = %1")
                                 .arg(started.order.id)),
             0);
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT amount_cents FROM orders WHERE id = %1").arg(started.order.id)),
        0);

    fixture.advance(250);
    const charging::server::ChargingOperationResult stopped =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY2(stopped.success, qPrintable(stopped.error.message));
    QVERIFY(stopped.order.status == charging::model::OrderStatus::WaitingPayment);
    QVERIFY(stopped.charger.status == charging::model::ChargerStatus::Available);
    QCOMPARE(stopped.order.durationSeconds, 500);
    QCOMPARE(stopped.order.energyWh, 1000);
    QCOMPARE(stopped.order.amountCents, 120);
    QCOMPARE(fixture.integer(QStringLiteral("SELECT total_charge_count FROM chargers WHERE id = %1")
                                 .arg(fixture.chargerId())),
             8);
    QCOMPARE(
        fixture.integer(QStringLiteral("SELECT total_charge_seconds FROM chargers WHERE id = %1")
                            .arg(fixture.chargerId())),
        600);

    fixture.advance(10);
    const charging::server::PaymentResult paid =
        fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY2(paid.success, qPrintable(paid.error.message));
    QVERIFY(paid.order.status == charging::model::OrderStatus::Completed);
    QCOMPARE(paid.balanceCents, 9880);

    QCOMPARE(fixture.text(QStringLiteral("SELECT status FROM reservations WHERE id = %1")
                              .arg(reserved.reservation.id)),
             QStringLiteral("FULFILLED"));
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(started.order.id)),
             QStringLiteral("COMPLETED"));
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("AVAILABLE"));
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT balance_cents FROM users WHERE id = %1").arg(fixture.userId())),
        9880);

    // A historical COMPLETED order remains safely idempotent after the same
    // charger has entered a newer workflow. Retrying the old order must not
    // release or otherwise mutate the new reservation.
    const auto nextReservation =
        fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(nextReservation.success);
    const auto oldStopAfterReuse =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(oldStopAfterReuse.success);
    QVERIFY(oldStopAfterReuse.idempotent);
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("RESERVED"));
    const auto oldPaymentAfterReuse =
        fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(oldPaymentAfterReuse.success);
    QVERIFY(oldPaymentAfterReuse.idempotent);
    QCOMPARE(oldPaymentAfterReuse.balanceCents, 9880);
}

void ChargingWorkflowTest::repeatedStopAndPaymentAreIdempotent()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);
    fixture.advance(1);
    const auto started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(started.success);
    fixture.advance(500);
    const auto firstStop =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(firstStop.success);
    QVERIFY(!firstStop.idempotent);

    fixture.advance(100);
    const auto secondStop =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(secondStop.success);
    QVERIFY(secondStop.idempotent);
    QCOMPARE(secondStop.order.durationSeconds, firstStop.order.durationSeconds);
    QCOMPARE(secondStop.order.energyWh, firstStop.order.energyWh);
    QCOMPARE(secondStop.order.amountCents, firstStop.order.amountCents);
    QCOMPARE(fixture.integer(QStringLiteral("SELECT total_charge_count FROM chargers WHERE id = %1")
                                 .arg(fixture.chargerId())),
             8);
    QCOMPARE(
        fixture.integer(QStringLiteral("SELECT total_charge_seconds FROM chargers WHERE id = %1")
                            .arg(fixture.chargerId())),
        600);

    const auto firstPayment = fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(firstPayment.success);
    QVERIFY(!firstPayment.idempotent);
    const auto secondPayment = fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(secondPayment.success);
    QVERIFY(secondPayment.idempotent);
    QCOMPARE(secondPayment.balanceCents, firstPayment.balanceCents);
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT balance_cents FROM users WHERE id = %1").arg(fixture.userId())),
        9880);
}

void ChargingWorkflowTest::cancellationUpdatesAllThreeRecordsAndIsIdempotent()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);
    fixture.advance(10);
    const auto cancelled =
        fixture.chargingService()->cancelReservation(fixture.userId(), reserved.reservation.id);
    QVERIFY2(cancelled.success, qPrintable(cancelled.error.message));
    QVERIFY(!cancelled.idempotent);
    QVERIFY(cancelled.reservation.status == charging::model::ReservationStatus::Cancelled);
    QVERIFY(cancelled.order.status == charging::model::OrderStatus::Cancelled);
    QVERIFY(cancelled.charger.status == charging::model::ChargerStatus::Available);
    QVERIFY(cancelled.reservation.endedAtUtc.isValid());

    const auto repeated =
        fixture.chargingService()->cancelReservation(fixture.userId(), reserved.reservation.id);
    QVERIFY(repeated.success);
    QVERIFY(repeated.idempotent);
    QCOMPARE(fixture.text(QStringLiteral("SELECT status FROM reservations WHERE id = %1")
                              .arg(reserved.reservation.id)),
             QStringLiteral("CANCELLED"));
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(reserved.order.id)),
             QStringLiteral("CANCELLED"));
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("AVAILABLE"));
    QCOMPARE(fixture.integer(QStringLiteral("SELECT total_charge_count FROM chargers WHERE id = %1")
                                 .arg(fixture.chargerId())),
             7);

    const auto paymentAfterCancellation =
        fixture.orderService()->pay(fixture.userId(), reserved.order.id);
    QVERIFY(!paymentAfterCancellation.success);
    QCOMPARE(paymentAfterCancellation.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));
}

void ChargingWorkflowTest::reservationExpiresAtTheFifteenMinuteBoundary()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);
    fixture.advance(charging::server::ChargingService::kReservationLifetimeSeconds);

    // Any workflow read applies due expiry first. Equality with expires_at is expired.
    const auto expired =
        fixture.chargingService()->chargingStatus(fixture.userId(), reserved.order.id);
    QVERIFY2(expired.success, qPrintable(expired.error.message));
    QVERIFY(expired.reservation.status == charging::model::ReservationStatus::Expired);
    QVERIFY(expired.order.status == charging::model::OrderStatus::Cancelled);
    QVERIFY(expired.charger.status == charging::model::ChargerStatus::Available);

    const auto startAfterExpiry =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(!startAfterExpiry.success);
    QCOMPARE(startAfterExpiry.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));
    QCOMPARE(fixture.text(QStringLiteral("SELECT status FROM reservations WHERE id = %1")
                              .arg(reserved.reservation.id)),
             QStringLiteral("EXPIRED"));
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(reserved.order.id)),
             QStringLiteral("CANCELLED"));
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("AVAILABLE"));

    const auto paymentAfterExpiry =
        fixture.orderService()->pay(fixture.userId(), reserved.order.id);
    QVERIFY(!paymentAfterExpiry.success);
    QCOMPARE(paymentAfterExpiry.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));
}

void ChargingWorkflowTest::reservationFreezesPriceAndUnfinishedOrderBlocksAnotherReservation()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);
    QCOMPARE(reserved.order.unitPriceCentsPerKwh, 120);

    const auto duplicateWhileReserved =
        fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(!duplicateWhileReserved.success);
    QCOMPARE(duplicateWhileReserved.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));

    QSqlQuery priceUpdate(fixture.sqlDatabase());
    QVERIFY2(priceUpdate.exec(QStringLiteral("UPDATE stations SET price_cents_per_kwh = 999")),
             qPrintable(priceUpdate.lastError().text()));
    fixture.advance(1);
    const auto started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(started.success);
    fixture.advance(500);
    const auto stopped =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(stopped.success);
    QCOMPARE(stopped.order.unitPriceCentsPerKwh, 120);
    QCOMPARE(stopped.order.amountCents, 120);

    const auto duplicateWhileWaitingPayment =
        fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(!duplicateWhileWaitingPayment.success);
    QCOMPARE(duplicateWhileWaitingPayment.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));
    QCOMPARE(fixture.integer(QStringLiteral("SELECT COUNT(*) FROM orders")), 1);
}

void ChargingWorkflowTest::rejectsUnavailableChargers()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto fault =
        fixture.chargingService()->reserve(fixture.userId(), fixture.faultChargerId());
    QVERIFY(!fault.success);
    QCOMPARE(fault.error.code, code(charging::protocol::error_code::kChargerNotAvailable));

    const auto offline =
        fixture.chargingService()->reserve(fixture.userId(), fixture.offlineChargerId());
    QVERIFY(!offline.success);
    QCOMPARE(offline.error.code, code(charging::protocol::error_code::kChargerNotAvailable));
    QCOMPARE(fixture.integer(QStringLiteral("SELECT COUNT(*) FROM reservations")), 0);
    QCOMPARE(fixture.integer(QStringLiteral("SELECT COUNT(*) FROM orders")), 0);
}

void ChargingWorkflowTest::rejectsNonOwnerAndIllegalTransitions()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);

    const auto paymentWhileReserved =
        fixture.orderService()->pay(fixture.userId(), reserved.order.id);
    QVERIFY(!paymentWhileReserved.success);
    QCOMPARE(paymentWhileReserved.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));

    const auto foreignStart =
        fixture.chargingService()->startCharging(fixture.otherUserId(), reserved.reservation.id);
    QVERIFY(!foreignStart.success);
    QCOMPARE(foreignStart.error.code, code(charging::protocol::error_code::kUnauthorized));

    fixture.advance(1);
    const auto started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(started.success);
    const auto repeatedStart =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(!repeatedStart.success);
    QCOMPARE(repeatedStart.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));

    const auto prematurePayment = fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(!prematurePayment.success);
    QCOMPARE(prematurePayment.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));

    const auto foreignStatus =
        fixture.chargingService()->chargingStatus(fixture.otherUserId(), started.order.id);
    QVERIFY(!foreignStatus.success);
    QCOMPARE(foreignStatus.error.code, code(charging::protocol::error_code::kUnauthorized));

    const auto foreignStop =
        fixture.chargingService()->stopCharging(fixture.otherUserId(), started.order.id);
    QVERIFY(!foreignStop.success);
    QCOMPARE(foreignStop.error.code, code(charging::protocol::error_code::kUnauthorized));

    fixture.advance(500);
    const auto ownerStop =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(ownerStop.success);
    const auto foreignPayment =
        fixture.orderService()->pay(fixture.otherUserId(), started.order.id);
    QVERIFY(!foreignPayment.success);
    QCOMPARE(foreignPayment.error.code, code(charging::protocol::error_code::kUnauthorized));
}

void ChargingWorkflowTest::insufficientBalanceLeavesOrderUnpaid()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(100, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);
    fixture.advance(1);
    const auto started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(started.success);
    fixture.advance(500);
    const auto stopped =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(stopped.success);
    QCOMPARE(stopped.order.amountCents, 120);

    const auto payment = fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(!payment.success);
    QCOMPARE(payment.error.code, code(charging::protocol::error_code::kInsufficientBalance));
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT balance_cents FROM users WHERE id = %1").arg(fixture.userId())),
        100);
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(started.order.id)),
             QStringLiteral("WAITING_PAYMENT"));
}

void ChargingWorkflowTest::rejectsClockRollbackAndInconsistentActiveState()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);

    fixture.advance(-1);
    const auto earlyStart =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(!earlyStart.success);
    QCOMPARE(earlyStart.error.code, code(charging::protocol::error_code::kInvalidStateTransition));
    QCOMPARE(fixture.text(QStringLiteral("SELECT status FROM reservations WHERE id = %1")
                              .arg(reserved.reservation.id)),
             QStringLiteral("ACTIVE"));

    const auto earlyCancel =
        fixture.chargingService()->cancelReservation(fixture.userId(), reserved.reservation.id);
    QVERIFY(!earlyCancel.success);
    QCOMPARE(earlyCancel.error.code, code(charging::protocol::error_code::kInvalidStateTransition));

    fixture.advance(1);
    const auto started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(started.success);

    // Simulate a future management bug that changes an active charger's row
    // without first ending its order. Reads must fail closed instead of
    // reporting CHARGING with a non-charging charger.
    QSqlQuery corrupt(fixture.sqlDatabase());
    QVERIFY2(corrupt.exec(QStringLiteral("UPDATE chargers SET status = 'AVAILABLE' WHERE id = %1")
                              .arg(fixture.chargerId())),
             qPrintable(corrupt.lastError().text()));
    const auto inconsistent =
        fixture.chargingService()->chargingStatus(fixture.userId(), started.order.id);
    QVERIFY(!inconsistent.success);
    QCOMPARE(inconsistent.error.code, code(charging::protocol::error_code::kDatabaseError));
    QVERIFY(corrupt.exec(QStringLiteral("UPDATE chargers SET status = 'CHARGING' WHERE id = %1")
                             .arg(fixture.chargerId())));

    fixture.advance(500);
    const auto stopped =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(stopped.success);

    fixture.advance(-1);
    const auto earlyPayment = fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(!earlyPayment.success);
    QCOMPARE(earlyPayment.error.code,
             code(charging::protocol::error_code::kInvalidStateTransition));
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT balance_cents FROM users WHERE id = %1").arg(fixture.userId())),
        10000);
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(started.order.id)),
             QStringLiteral("WAITING_PAYMENT"));

    fixture.advance(1);
    QVERIFY2(corrupt.exec(QStringLiteral("UPDATE reservations SET status = 'CANCELLED' "
                                         "WHERE id = %1")
                              .arg(reserved.reservation.id)),
             qPrintable(corrupt.lastError().text()));
    const auto inconsistentPayment =
        fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(!inconsistentPayment.success);
    QCOMPARE(inconsistentPayment.error.code, code(charging::protocol::error_code::kDatabaseError));
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT balance_cents FROM users WHERE id = %1").arg(fixture.userId())),
        10000);
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(started.order.id)),
             QStringLiteral("WAITING_PAYMENT"));
}

void ChargingWorkflowTest::reserveStartAndPaymentFailuresRollBack()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    QSqlQuery query(fixture.sqlDatabase());
    QVERIFY2(query.exec(QStringLiteral("CREATE TRIGGER reject_test_reserve_order "
                                       "BEFORE INSERT ON orders WHEN NEW.status = 'RESERVED' "
                                       "BEGIN SELECT RAISE(ABORT, 'forced reserve failure'); END")),
             qPrintable(query.lastError().text()));
    const auto failedReserve =
        fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(!failedReserve.success);
    QCOMPARE(failedReserve.error.code, code(charging::protocol::error_code::kDatabaseError));
    QCOMPARE(fixture.integer(QStringLiteral("SELECT COUNT(*) FROM reservations")), 0);
    QCOMPARE(fixture.integer(QStringLiteral("SELECT COUNT(*) FROM orders")), 0);
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("AVAILABLE"));
    QVERIFY(query.exec(QStringLiteral("DROP TRIGGER reject_test_reserve_order")));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);
    QVERIFY2(query.exec(QStringLiteral("CREATE TRIGGER reject_test_start_charger "
                                       "BEFORE UPDATE OF status ON chargers "
                                       "WHEN OLD.status = 'RESERVED' AND NEW.status = 'CHARGING' "
                                       "BEGIN SELECT RAISE(ABORT, 'forced start failure'); END")),
             qPrintable(query.lastError().text()));
    fixture.advance(1);
    const auto failedStart =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(!failedStart.success);
    QCOMPARE(failedStart.error.code, code(charging::protocol::error_code::kDatabaseError));
    QCOMPARE(fixture.text(QStringLiteral("SELECT status FROM reservations WHERE id = %1")
                              .arg(reserved.reservation.id)),
             QStringLiteral("ACTIVE"));
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(reserved.order.id)),
             QStringLiteral("RESERVED"));
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("RESERVED"));
    QVERIFY(query.exec(QStringLiteral("DROP TRIGGER reject_test_start_charger")));

    const auto started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(started.success);
    fixture.advance(500);
    const auto stopped =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(stopped.success);

    QVERIFY2(query.exec(
                 QStringLiteral("CREATE TRIGGER reject_test_payment_order "
                                "BEFORE UPDATE OF status ON orders "
                                "WHEN OLD.status = 'WAITING_PAYMENT' AND NEW.status = 'COMPLETED' "
                                "BEGIN SELECT RAISE(ABORT, 'forced payment failure'); END")),
             qPrintable(query.lastError().text()));
    const auto failedPayment = fixture.orderService()->pay(fixture.userId(), started.order.id);
    QVERIFY(!failedPayment.success);
    QCOMPARE(failedPayment.error.code, code(charging::protocol::error_code::kDatabaseError));
    QCOMPARE(
        fixture.integer(
            QStringLiteral("SELECT balance_cents FROM users WHERE id = %1").arg(fixture.userId())),
        10000);
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(started.order.id)),
             QStringLiteral("WAITING_PAYMENT"));
}

void ChargingWorkflowTest::failedStopRollsBackEveryTable()
{
    WorkflowFixture fixture;
    QString error;
    QVERIFY2(fixture.start(10000, 120, &error), qPrintable(error));

    const auto reserved = fixture.chargingService()->reserve(fixture.userId(), fixture.chargerId());
    QVERIFY(reserved.success);
    fixture.advance(1);
    const auto started =
        fixture.chargingService()->startCharging(fixture.userId(), reserved.reservation.id);
    QVERIFY(started.success);

    QSqlQuery trigger(fixture.sqlDatabase());
    QVERIFY2(
        trigger.exec(QStringLiteral("CREATE TRIGGER reject_test_stop "
                                    "BEFORE UPDATE OF status ON chargers "
                                    "WHEN OLD.status = 'CHARGING' AND NEW.status = 'AVAILABLE' "
                                    "BEGIN SELECT RAISE(ABORT, 'forced stop failure'); END")),
        qPrintable(trigger.lastError().text()));

    fixture.advance(500);
    const auto stopped =
        fixture.chargingService()->stopCharging(fixture.userId(), started.order.id);
    QVERIFY(!stopped.success);
    QCOMPARE(stopped.error.code, code(charging::protocol::error_code::kDatabaseError));
    QCOMPARE(fixture.text(
                 QStringLiteral("SELECT status FROM orders WHERE id = %1").arg(started.order.id)),
             QStringLiteral("CHARGING"));
    QCOMPARE(fixture.integer(QStringLiteral("SELECT duration_seconds FROM orders WHERE id = %1")
                                 .arg(started.order.id)),
             0);
    QCOMPARE(
        fixture.text(
            QStringLiteral("SELECT status FROM chargers WHERE id = %1").arg(fixture.chargerId())),
        QStringLiteral("CHARGING"));
    QCOMPARE(fixture.integer(QStringLiteral("SELECT total_charge_count FROM chargers WHERE id = %1")
                                 .arg(fixture.chargerId())),
             7);
    QCOMPARE(
        fixture.integer(QStringLiteral("SELECT total_charge_seconds FROM chargers WHERE id = %1")
                            .arg(fixture.chargerId())),
        100);
}

QTEST_GUILESS_MAIN(ChargingWorkflowTest)

#include "tst_charging_workflow.moc"
