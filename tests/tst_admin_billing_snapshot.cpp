#include "admin_order_billing.h"
#include "billing_service.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "database_connection.h"
#include "database_maintenance.h"
#include "order_repository.h"
#include "order_service.h"

#include <QJsonArray>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace charging::server;

class AdminBillingSnapshotTest final : public QObject
{
    Q_OBJECT
private slots:
    void snapshotSurvivesPriceChangesAndPayment();
    void legacyBillingIsUnavailable();
    void snapshotInsertFailureRollsBackReservation();
    void meterRejectsStaleSamples();
    void legacyMigrationIsAtomic_data();
    void legacyMigrationIsAtomic();
    void exactAmountBoundaries();
};

void AdminBillingSnapshotTest::snapshotSurvivesPriceChangesAndPayment()
{
    DatabaseConnection db;
    QString error;
    QVERIFY2(db.open(QStringLiteral(":memory:"), true, &error), qPrintable(error));
    QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-09T00:00:00.000Z"), Qt::ISODateWithMs);
    ChargingRepository repository(db.database());
    BillingService billing;
    ChargingService service(&repository, &billing, [&now]() { return now; });
    auto reserved = service.reserve(1, 1);
    QVERIFY(reserved.success);
    QJsonObject dto;
    QVERIFY(orderBillingDto(db.database(), reserved.order.id, &dto, &error));
    QCOMPARE(dto.value("billingAvailability").toString(), QStringLiteral("AVAILABLE"));
    QCOMPARE(dto.value("pricingSnapshot").toObject().value("version").toString(),
             QStringLiteral("energy-only-v1"));
    QSqlQuery price(db.database());
    QVERIFY(price.exec(QStringLiteral("UPDATE stations SET price_cents_per_kwh = 999 WHERE id = 1")));
    QVERIFY(service.startCharging(1, reserved.reservation.id).success);
    now = now.addSecs(60);
    const auto sample = service.chargingStatus(1, reserved.order.id);
    QVERIFY(sample.success);
    QCOMPARE(sample.order.energyWh, qint64(2000));
    QCOMPARE(sample.order.amountCents, qint64(240));
    QVERIFY(orderBillingDto(db.database(), reserved.order.id, &dto, &error));
    QVERIFY(dto.value("estimated").toBool());
    auto fee = dto.value("feeBreakdown").toObject();
    QCOMPARE(fee.value("payableCents").toInt(), 240);
    QCOMPARE(fee.value("paidCents").toInt(), 0);
    for (const auto* field : {"serviceFeeCents", "parkingFeeCents", "discountCents"})
        QCOMPARE(fee.value(QLatin1String(field)).toInt(-1), 0);
    const auto segment = dto.value("pricingSnapshot").toObject().value("segments").toArray().at(0).toObject();
    QCOMPARE(segment.value("unitPriceCentsPerKwh").toInt(), 120);
    QCOMPARE(segment.value("endAt").toString(), now.toString(Qt::ISODateWithMs));
    QVERIFY(service.stopCharging(1, reserved.order.id).success);
    OrderRepository orders(db.database());
    OrderService payment(&orders, [&now]() { return now; });
    QVERIFY(payment.pay(1, reserved.order.id).success);
    QVERIFY(orderBillingDto(db.database(), reserved.order.id, &dto, &error));
    QVERIFY(!dto.value("estimated").toBool());
    fee = dto.value("feeBreakdown").toObject();
    QCOMPARE(fee.value("paidCents").toInt(), 240);
    QCOMPARE(fee.value("currency").toString(), QStringLiteral("CNY"));
    QVERIFY(payment.pay(1, reserved.order.id).idempotent);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString backupPath = directory.filePath(QStringLiteral("snapshot-backup.sqlite"));
    const auto backup = DatabaseMaintenance::backup(db.database(), backupPath);
    QVERIFY2(backup.ok, qPrintable(backup.errorMessage));
    DatabaseConnection restored;
    QVERIFY2(restored.open(backupPath, false, &error), qPrintable(error));
    QJsonObject restoredDto;
    QVERIFY(orderBillingDto(restored.database(), reserved.order.id, &restoredDto, &error));
    QCOMPARE(restoredDto, dto);
}

void AdminBillingSnapshotTest::legacyBillingIsUnavailable()
{
    DatabaseConnection db;
    QString error;
    QVERIFY2(db.open(QStringLiteral(":memory:"), true, &error), qPrintable(error));
    QSqlQuery query(db.database());
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO orders(order_no,user_id,charger_id,status,unit_price_cents_per_kwh,"
        "energy_wh,amount_cents,started_at,stopped_at,paid_at) VALUES "
        "('HISTORY',1,1,'COMPLETED',120,1000,120,'2026-09-01T00:00:00.000Z',"
        "'2026-09-01T00:01:00.000Z','2026-09-01T00:02:00.000Z')")));
    QJsonObject dto;
    QVERIFY(orderBillingDto(db.database(), query.lastInsertId().toLongLong(), &dto, &error));
    QCOMPARE(dto.value("billingAvailability").toString(), QStringLiteral("UNAVAILABLE"));
    QVERIFY(dto.value("feeBreakdown").isNull());
    QVERIFY(dto.value("pricingSnapshot").isNull());
}

void AdminBillingSnapshotTest::snapshotInsertFailureRollsBackReservation()
{
    DatabaseConnection db;
    QString error;
    QVERIFY(db.open(QStringLiteral(":memory:"), true, &error));
    QSqlQuery query(db.database());
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TRIGGER fail_snapshot BEFORE INSERT ON order_pricing_snapshots "
        "BEGIN SELECT RAISE(ABORT,'injected failure'); END")));
    ChargingRepository repository(db.database());
    BillingService billing;
    ChargingService service(&repository, &billing);
    QVERIFY(!service.reserve(1, 1).success);
    QVERIFY(query.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM orders), "
                                     "(SELECT COUNT(*) FROM reservations), status FROM chargers WHERE id = 1")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    QCOMPARE(query.value(1).toInt(), 0);
    QCOMPARE(query.value(2).toString(), QStringLiteral("AVAILABLE"));
}

void AdminBillingSnapshotTest::meterRejectsStaleSamples()
{
    DatabaseConnection db;
    QString error;
    QVERIFY(db.open(QStringLiteral(":memory:"), true, &error));
    QDateTime now = QDateTime::currentDateTimeUtc();
    ChargingRepository repository(db.database());
    BillingService billing;
    ChargingService service(&repository, &billing, [&now]() { return now; });
    const auto reserved = service.reserve(1, 1);
    QVERIFY(reserved.success);
    QVERIFY(service.startCharging(1, reserved.reservation.id).success);
    now = now.addSecs(60);
    QVERIFY(service.chargingStatus(1, reserved.order.id).success);
    QVERIFY(!repository.recordTelemetry(1, reserved.order.id, now.addSecs(-30), 120000, 30, 1000, 120));
    QVERIFY(!repository.recordTelemetry(1, reserved.order.id, now, 120000, 60, 2000, 999));
    QJsonObject dto;
    QVERIFY(orderBillingDto(db.database(), reserved.order.id, &dto));
    QCOMPARE(dto.value("feeBreakdown").toObject().value("payableCents").toInt(), 240);
    // A same-second clock rollback must not move the final sample backwards,
    // even though integer duration and Wh would otherwise remain unchanged.
    QVERIFY(repository.recordTelemetry(1, reserved.order.id, now.addMSecs(900), 120000, 60, 2000, 240));
    QVERIFY(!service.stopCharging(1, reserved.order.id).success);
    now = now.addSecs(1);
    QVERIFY(service.stopCharging(1, reserved.order.id).success);
    QVERIFY(!repository.recordTelemetry(1, reserved.order.id, now.addSecs(1), 120000, 61, 2033, 244));
}

void AdminBillingSnapshotTest::legacyMigrationIsAtomic_data()
{
    QTest::addColumn<bool>("injectFailure");
    QTest::newRow("preserve-and-repeat") << false;
    QTest::newRow("rollback-all-schema-changes") << true;
}

void AdminBillingSnapshotTest::legacyMigrationIsAtomic()
{
    QFETCH(bool, injectFailure);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("legacy.sqlite"));
    QString error;
    DatabaseConnection db;
    QVERIFY2(db.open(path, true, &error), qPrintable(error));
    {
        QSqlQuery query(db.database());
        for (const auto* sql : {
                 "DROP TABLE queue_entries", "DROP TABLE repair_timeline", "DROP TABLE repair_reports",
                 "DROP TABLE repair_operations", "DROP TABLE order_charge_targets",
                 "DROP TABLE order_pricing_snapshots", "DROP TABLE charger_exceptions",
                 "ALTER TABLE stations DROP COLUMN city", "ALTER TABLE stations DROP COLUMN district",
                 "ALTER TABLE stations DROP COLUMN contact_name", "ALTER TABLE stations DROP COLUMN contact_phone",
                 "ALTER TABLE orders DROP COLUMN telemetry_captured_at", "ALTER TABLE orders DROP COLUMN telemetry_power_watts",
                 "ALTER TABLE orders DROP COLUMN stop_reason",
                 "UPDATE users SET nickname = 'preserved-name', balance_cents = 12345 WHERE id = 1",
                 "PRAGMA user_version = 3"})
            QVERIFY2(query.exec(QString::fromLatin1(sql)), qPrintable(query.lastError().text()));
        if (injectFailure)
            QVERIFY(query.exec(QStringLiteral("CREATE VIEW charger_exceptions AS SELECT 1 AS id")));
    }
    db.close();
    const bool opened = db.open(path, false, &error);
    QCOMPARE(opened, !injectFailure);
    if (!injectFailure) {
        QSqlQuery query(db.database());
        QVERIFY(query.exec(QStringLiteral("SELECT city,district,contact_name,contact_phone FROM stations WHERE id=1")));
        QVERIFY(query.next());
        for (int i = 0; i < 4; ++i) QVERIFY(query.value(i).isNull());
        query.finish();
        db.close();
        QVERIFY2(db.open(path, false, &error), qPrintable(error));
        db.close();
        QVERIFY2(DatabaseMaintenance::validate(path).ok, "Migrated schema must pass strict backup validation");
    }
    const QString name = QStringLiteral("migration-verification-%1").arg(injectFailure);
    {
        auto verification = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        verification.setDatabaseName(path);
        QVERIFY(verification.open());
        QSqlQuery query(verification);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), injectFailure ? 3 : 5);
        QVERIFY(query.exec(QStringLiteral("SELECT nickname,balance_cents FROM users WHERE id=1")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("preserved-name"));
        QCOMPARE(query.value(1).toInt(), 12345);
        QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name='order_pricing_snapshots'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), injectFailure ? 0 : 1);
        verification.close();
    }
    QSqlDatabase::removeDatabase(name);
}

void AdminBillingSnapshotTest::exactAmountBoundaries()
{
    qint64 value = 0;
    QVERIFY(calculateEnergyFeeCents(1, 500, &value));
    QCOMPARE(value, qint64(1));
    QVERIFY(calculateEnergyFeeCents(1000, 9007199254740991LL, &value));
    QCOMPARE(value, 9007199254740991LL);
    QVERIFY(!calculateEnergyFeeCents(1001, 9007199254740991LL, &value));
    QVERIFY(!calculateEnergyFeeCents(-1, 120, &value));
}

QTEST_GUILESS_MAIN(AdminBillingSnapshotTest)
#include "tst_admin_billing_snapshot.moc"
