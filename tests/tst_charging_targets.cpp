#include "billing_service.h"
#include "charging_repository.h"
#include "charging_service.h"
#include "charging_target_repository.h"
#include "database_connection.h"
#include "admin_order_billing.h"
#include "admin_repository.h"
#include "order_repository.h"
#include "user_api_repository.h"
#include "user_api_service.h"

#include <QJsonArray>
#include <QSqlQuery>
#include <QSqlError>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace charging::server;
namespace {
QJsonObject target(const QString& type, qint64 value)
{
    return {{QStringLiteral("type"), type}, {QStringLiteral("value"), value}};
}

class Fixture final
{
public:
    QTemporaryDir directory;
    DatabaseConnection database;
    QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-09T08:00:00.000Z"), Qt::ISODateWithMs);
    BillingService billing;
    std::unique_ptr<ChargingRepository> repository;
    std::unique_ptr<ChargingService> service;
    QString error;
    bool open(int power = 180000, qint64 price = 120)
    {
        if (!database.open(directory.filePath(QStringLiteral("targets.sqlite3")), false, &error)) return false;
        if (!exec(QStringLiteral("INSERT INTO users(id,phone,nickname,balance_cents,status) "
                                 "VALUES(1,'13800000001','目标用户',10000,'ACTIVE')"))
            || !exec(QStringLiteral("INSERT INTO stations(id,code,name,address,latitude,longitude,price_cents_per_kwh,status) "
                                     "VALUES(1,'STA-TARGET','目标测试站','测试地址',38.9,121.5,%1,'ACTIVE')").arg(price))
            || !exec(QStringLiteral("INSERT INTO chargers(id,station_id,code,type,power_watts,status) "
                                     "VALUES(1,1,'CHG-TARGET','FAST',%1,'AVAILABLE')").arg(power))) return false;
        restartService();
        return true;
    }
    void restartService()
    {
        repository = std::make_unique<ChargingRepository>(database.database());
        service = std::make_unique<ChargingService>(repository.get(), &billing, [this] { return now; });
    }
    bool exec(const QString& sql)
    {
        QSqlQuery query(database.database());
        if (query.exec(sql)) return true;
        error = query.lastError().text();
        return false;
    }
    QVariant scalar(const QString& sql)
    {
        QSqlQuery query(database.database());
        if (!query.exec(sql) || !query.next()) return {};
        return query.value(0);
    }
    ChargingOperationResult begin(const QJsonObject& selected)
    {
        const auto reserved = service->reserve(1, 1);
        if (!reserved.success) return reserved;
        return service->startCharging(1, reserved.reservation.id, selected);
    }
};
}

class ChargingTargetsTest final : public QObject
{
    Q_OBJECT
private slots:
    void integerSamples_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<qint64>("value");
        QTest::addColumn<int>("power");
        QTest::addColumn<qint64>("price");
        QTest::addColumn<qint64>("elapsed");
        QTest::addColumn<qint64>("energy");
        QTest::addColumn<qint64>("amount");
        QTest::addColumn<bool>("reached");
        QTest::newRow("20-yuan-non-second-boundary") << QStringLiteral("AMOUNT") << qint64(2000) << 180000 << qint64(120) << qint64(5000) << qint64(16663) << qint64(2000) << true;
        QTest::newRow("one-cent") << QStringLiteral("AMOUNT") << qint64(1) << 180000 << qint64(120) << qint64(1) << qint64(5) << qint64(1) << true;
        QTest::newRow("budget-below-one-wh-price") << QStringLiteral("AMOUNT") << qint64(1) << 180000 << qint64(2000) << qint64(1) << qint64(0) << qint64(0) << true;
        QTest::newRow("unrepresentable-budget-stops-before") << QStringLiteral("AMOUNT") << qint64(3) << 180000 << qint64(2000) << qint64(1) << qint64(1) << qint64(2) << true;
        QTest::newRow("energy-partial-final-second") << QStringLiteral("ENERGY") << qint64(123) << 180000 << qint64(120) << qint64(10) << qint64(123) << qint64(15) << true;
        QTest::newRow("ten-kwh") << QStringLiteral("ENERGY") << qint64(10000) << 180000 << qint64(120) << qint64(10000) << qint64(10000) << qint64(1200) << true;
        QTest::newRow("thirty-minutes") << QStringLiteral("DURATION") << qint64(1800) << 7000 << qint64(120) << qint64(1900) << qint64(3500) << qint64(420) << true;
        QTest::newRow("free-energy") << QStringLiteral("ENERGY") << qint64(10000) << 180000 << qint64(0) << qint64(201) << qint64(10000) << qint64(0) << true;
        QTest::newRow("before-target") << QStringLiteral("ENERGY") << qint64(10000) << 180000 << qint64(120) << qint64(1) << qint64(50) << qint64(6) << false;
    }

    void integerSamples()
    {
        QFETCH(QString, type); QFETCH(qint64, value); QFETCH(int, power); QFETCH(qint64, price);
        QFETCH(qint64, elapsed); QFETCH(qint64, energy); QFETCH(qint64, amount); QFETCH(bool, reached);
        const auto sample = calculateTargetSample(power, elapsed, price, target(type, value));
        QVERIFY(sample.success);
        QCOMPARE(sample.energyWh, energy);
        QCOMPARE(sample.amountCents, amount);
        QCOMPARE(sample.reached, reached);
        QVERIFY(sample.durationSeconds <= elapsed);
        if (type == QStringLiteral("AMOUNT")) QVERIFY(sample.amountCents <= value);
    }

    void rejectsMalformedTargets()
    {
        QVERIFY(validateChargingTarget({}));
        QVERIFY(!validateChargingTarget(target(QStringLiteral("MANUAL"), 20)));
        QVERIFY(!validateChargingTarget(target(QStringLiteral("ENERGY"), 0)));
        QVERIFY(!validateChargingTarget({{QStringLiteral("type"), QStringLiteral("AMOUNT")}, {QStringLiteral("value"), 0.5}}));
        QVERIFY(!validateChargingTarget({{QStringLiteral("type"), QStringLiteral("AMOUNT")}, {QStringLiteral("value"), QStringLiteral("20")}}));
        auto extra = target(QStringLiteral("DURATION"), 30);
        extra.insert(QStringLiteral("price"), 1);
        QVERIFY(!validateChargingTarget(extra));
        QVERIFY(!calculateTargetSample(180000, 10, 0, target(QStringLiteral("AMOUNT"), 20)).success);
    }

    void noClientPollingStopsAndPersistsProgress()
    {
        Fixture f;
        QVERIFY2(f.open(), qPrintable(f.error));
        const auto started = f.begin(target(QStringLiteral("AMOUNT"), 2000));
        QVERIFY2(started.success, qPrintable(started.error.message));
        QCOMPARE(started.order.target.value(QStringLiteral("remainingValue")).toInt(), 2000);
        f.now = f.now.addSecs(10);
        QVERIFY(f.service->advanceTargets(&f.error));
        QCOMPARE(f.scalar(QStringLiteral("SELECT amount_cents FROM orders")).toLongLong(), qint64(60));
        f.now = f.now.addSecs(10000);
        QVERIFY2(f.service->advanceTargets(&f.error), qPrintable(f.error));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM orders")).toString(), QStringLiteral("WAITING_PAYMENT"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT amount_cents FROM orders")).toLongLong(), qint64(2000));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM chargers")).toString(), QStringLiteral("AVAILABLE"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT stop_reason FROM orders")).toString(), QStringLiteral("TARGET_AMOUNT"));
        const auto final = f.service->chargingStatus(1, started.order.id);
        QVERIFY(final.success);
        QVERIFY(final.order.target.value(QStringLiteral("reached")).toBool());
        QCOMPARE(final.order.target.value(QStringLiteral("remainingValue")).toInt(), 0);
        QCOMPARE(final.currentPowerWatts, 0);
        QJsonObject billing;
        QVERIFY(orderBillingDto(f.database.database(), started.order.id, &billing, &f.error));
        QCOMPARE(billing.value(QStringLiteral("billingAvailability")).toString(), QStringLiteral("AVAILABLE"));
        QVERIFY(f.service->advanceTargets());
        QCOMPARE(f.scalar(QStringLiteral("SELECT total_charge_count FROM chargers")).toInt(), 1);
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM notifications WHERE type='CHARGING_STOPPED'")).toInt(), 1);
    }

    void restartUsesPersistedTargetAndFrozenPrice()
    {
        Fixture f;
        QVERIFY2(f.open(), qPrintable(f.error));
        const auto started = f.begin(target(QStringLiteral("ENERGY"), 123));
        QVERIFY(started.success);
        QVERIFY(f.exec(QStringLiteral("UPDATE stations SET price_cents_per_kwh=9999")));
        f.now = f.now.addSecs(1000000);
        f.service.reset();
        f.repository.reset();
        f.database.close();
        QVERIFY2(f.database.open(f.directory.filePath(QStringLiteral("targets.sqlite3")), false, &f.error),
                 qPrintable(f.error));
        f.restartService();
        QVERIFY2(f.service->advanceTargets(&f.error), qPrintable(f.error));
        const auto final = f.service->chargingStatus(1, started.order.id);
        QVERIFY(final.success);
        QCOMPARE(final.order.energyWh, qint64(123));
        QCOMPARE(final.order.amountCents, qint64(15));
        QCOMPARE(final.order.durationSeconds, qint64(3));
        QCOMPARE(final.order.stopReason, QStringLiteral("TARGET_ENERGY"));
    }

    void manualStopBeforeGoalAndRepeatAreStable()
    {
        Fixture f;
        QVERIFY2(f.open(), qPrintable(f.error));
        const auto started = f.begin(target(QStringLiteral("DURATION"), 1800));
        QVERIFY(started.success);
        f.now = f.now.addSecs(60);
        const auto stopped = f.service->stopCharging(1, started.order.id);
        QVERIFY(stopped.success);
        QCOMPARE(stopped.order.stopReason, QStringLiteral("MANUAL"));
        QCOMPARE(stopped.order.durationSeconds, qint64(60));
        QCOMPARE(stopped.order.target.value(QStringLiteral("remainingValue")).toInt(), 1740);
        QVERIFY(!stopped.order.target.value(QStringLiteral("reached")).toBool());
        f.now = f.now.addSecs(9999);
        const auto repeated = f.service->stopCharging(1, started.order.id);
        QVERIFY(repeated.success);
        QVERIFY(repeated.idempotent);
        QCOMPARE(repeated.order.amountCents, stopped.order.amountCents);
        QCOMPARE(repeated.order.stopReason, QStringLiteral("MANUAL"));
    }

    void targetAndReasonSurviveUserAdminAndPaymentDtos()
    {
        Fixture f;
        QVERIFY2(f.open(), qPrintable(f.error));
        const auto started = f.begin(target(QStringLiteral("DURATION"), 2));
        QVERIFY(started.success);
        const auto replay = f.service->startCharging(1, started.reservation.id, target(QStringLiteral("DURATION"), 2));
        QVERIFY(replay.success && replay.idempotent);
        QVERIFY(!f.service->startCharging(1, started.reservation.id, target(QStringLiteral("ENERGY"), 2)).success);
        f.now = f.now.addSecs(3);
        QVERIFY(f.service->advanceTargets());
        UserApiRepository userRepository(f.database.database());
        UserApiService userService(&userRepository, [&f] { return f.now; });
        const auto reply = userService.handle(QStringLiteral("GET_ORDERS"), {}, 1);
        QVERIFY2(reply.success, qPrintable(reply.error.message));
        const auto userOrder = reply.data.value(QStringLiteral("orders")).toArray().first().toObject();
        QCOMPARE(userOrder.value(QStringLiteral("stopReason")).toString(), QStringLiteral("TARGET_DURATION"));
        QCOMPARE(userOrder.value(QStringLiteral("target")).toObject().value(QStringLiteral("completedValue")).toInt(), 2);
        AdminRepository admin(f.database.database());
        const auto adminOrder = admin.read(QStringLiteral("orders"), {{QStringLiteral("id"), QString::number(started.order.id)}})
                                    .value(QStringLiteral("item")).toObject();
        QCOMPARE(adminOrder.value(QStringLiteral("target")), userOrder.value(QStringLiteral("target")));
        QCOMPARE(adminOrder.value(QStringLiteral("stopReason")), userOrder.value(QStringLiteral("stopReason")));
        OrderRepository orders(f.database.database());
        const auto list = orders.list({});
        QVERIFY(list.ok && list.orders.size() == 1);
        QCOMPARE(list.orders.first().order.target, userOrder.value(QStringLiteral("target")).toObject());
        const auto paid = orders.pay(1, started.order.id, f.now);
        QVERIFY2(paid.ok, qPrintable(paid.diagnostic));
        QCOMPARE(paid.order.stopReason, QStringLiteral("TARGET_DURATION"));
        QCOMPARE(paid.order.target, list.orders.first().order.target);
    }

    void stopRequestAfterThresholdCannotOvershoot()
    {
        Fixture f;
        QVERIFY2(f.open(), qPrintable(f.error));
        const auto started = f.begin(target(QStringLiteral("AMOUNT"), 1));
        QVERIFY(started.success);
        f.now = f.now.addSecs(3600);
        const auto stopped = f.service->stopCharging(1, started.order.id);
        QVERIFY(stopped.success);
        QCOMPARE(stopped.order.amountCents, qint64(1));
        QCOMPARE(stopped.order.stopReason, QStringLiteral("TARGET_AMOUNT"));
    }

    void freePriceRejectsAmountButKeepsReservation()
    {
        Fixture f;
        QVERIFY2(f.open(180000, 0), qPrintable(f.error));
        const auto start = f.begin(target(QStringLiteral("AMOUNT"), 100));
        QVERIFY(!start.success);
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM reservations")).toString(), QStringLiteral("ACTIVE"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM chargers")).toString(), QStringLiteral("RESERVED"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM order_charge_targets")).toInt(), 0);
        const auto started = f.service->startCharging(1, 1, target(QStringLiteral("DURATION"), 1));
        QVERIFY(started.success);
        f.now = f.now.addSecs(2);
        QVERIFY(f.service->advanceTargets());
        QCOMPARE(f.scalar(QStringLiteral("SELECT amount_cents FROM orders")).toLongLong(), qint64(0));
    }

    void targetInsertFailureRollsBackEntireStart()
    {
        Fixture f;
        QVERIFY2(f.open(), qPrintable(f.error));
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER reject_target BEFORE INSERT ON order_charge_targets "
                                       "BEGIN SELECT RAISE(ABORT, 'injected target failure'); END")));
        const auto started = f.begin(target(QStringLiteral("ENERGY"), 1000));
        QVERIFY(!started.success);
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM orders")).toString(), QStringLiteral("RESERVED"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM reservations")).toString(), QStringLiteral("ACTIVE"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM chargers")).toString(), QStringLiteral("RESERVED"));
        QVERIFY(f.scalar(QStringLiteral("SELECT started_at FROM orders")).isNull());
    }

    void stopFailureDoesNotWriteOverBudgetOrRelease()
    {
        Fixture f;
        QVERIFY2(f.open(), qPrintable(f.error));
        QVERIFY(f.begin(target(QStringLiteral("AMOUNT"), 20)).success);
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER reject_release BEFORE UPDATE OF status ON chargers "
                                       "WHEN NEW.status='AVAILABLE' BEGIN SELECT RAISE(ABORT, 'injected stop failure'); END")));
        f.now = f.now.addSecs(10000);
        QVERIFY(!f.service->advanceTargets());
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM orders")).toString(), QStringLiteral("CHARGING"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT amount_cents FROM orders")).toLongLong(), qint64(0));
        QVERIFY(f.scalar(QStringLiteral("SELECT stop_reason FROM orders")).isNull());
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM chargers")).toString(), QStringLiteral("CHARGING"));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER reject_release")));
        QVERIFY(f.service->advanceTargets());
        QVERIFY(f.scalar(QStringLiteral("SELECT amount_cents FROM orders")).toLongLong() <= 20);
    }
};

QTEST_GUILESS_MAIN(ChargingTargetsTest)
#include "tst_charging_targets.moc"
