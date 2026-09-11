#include "admin_repository.h"
#include "charging_repository.h"
#include "database_connection.h"

#include <QJsonArray>
#include <QSqlQuery>
#include <QtTest>

using namespace charging::server;

namespace {
QDateTime instant(const char* text)
{
    return QDateTime::fromString(QString::fromLatin1(text), Qt::ISODateWithMs);
}

QString storedStatus(const QSqlDatabase& database, const QString& table, qint64 id)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT status FROM %1 WHERE id=?").arg(table));
    query.addBindValue(id);
    if (!query.exec() || !query.next())
        return {};
    return query.value(0).toString();
}
} // namespace

class DeliveryDataContractTest final : public QObject
{
    Q_OBJECT
private slots:
    void userRegistrationTimeIsNotLastUpdate();
    void stationOnlineRateUsesAllNonOfflineStates();
    void stationEmptyAndFilteredResultsUseSameContract();
    void expiryUpdatesThreeTablesAndLeavesLiveReservations();
    void expiryRollsBackAllTablesOnFailure();
    void expiryRejectsInvalidContext();
};

void DeliveryDataContractTest::userRegistrationTimeIsNotLastUpdate()
{
    DatabaseConnection connection;
    QString diagnostic;
    QVERIFY2(connection.open(":memory:", true, &diagnostic), qPrintable(diagnostic));
    QSqlQuery mutation(connection.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "UPDATE users SET updated_at='2026-09-08T01:00:00.000Z', status='FROZEN' WHERE id=1")));
    AdminRepository repository(connection.database());
    const auto detail = repository.read("users", {{"id", "1"}}).value("item").toObject();
    const auto list = repository.read("users", {{"status", "FROZEN"}});
    QCOMPARE(list.value("total").toInt(), 1);
    QCOMPARE(list.value("items").toArray().first().toObject(), detail);
    QCOMPARE(detail.value("createdAt").toString(), QString("2026-09-01T00:00:00.000Z"));
    QCOMPARE(detail.value("createdAtUtc"), detail.value("createdAt"));
    QVERIFY(detail.value("createdAt") != detail.value("updatedAt"));
    QCOMPARE(detail.value("phone").toString(), QString("138****8000"));
    QCOMPARE(detail.value("id").toString(), QString("1"));
}

void DeliveryDataContractTest::stationOnlineRateUsesAllNonOfflineStates()
{
    DatabaseConnection connection;
    QString diagnostic;
    QVERIFY2(connection.open(":memory:", true, &diagnostic), qPrintable(diagnostic));
    QSqlQuery mutation(connection.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "UPDATE chargers SET status='RESERVED' WHERE id=2")));
    QVERIFY(mutation.exec(QStringLiteral(
        "INSERT INTO chargers(id,station_id,code,type,power_watts,status) VALUES "
        "(8,1,'CONTRACT-CHARGING','FAST',60000,'CHARGING'),"
        "(9,1,'CONTRACT-OFFLINE','FAST',60000,'OFFLINE')")));
    AdminRepository repository(connection.database());
    const auto detail = repository.read("stations", {{"id", "1"}}).value("item").toObject();
    const auto listed = repository.read("stations", {{"keyword", "STA-DEMO-001"}});
    QCOMPARE(listed.value("total").toInt(), 1);
    QCOMPARE(listed.value("items").toArray().first().toObject(), detail);
    QCOMPARE(detail.value("totalChargers").toInt(), 5);
    QCOMPARE(detail.value("availableChargers").toInt(), 1);
    QCOMPARE(detail.value("onlineChargerCount").toInt(), 4);
    QCOMPARE(detail.value("onlineRatePercent").toDouble(), 80.0);
    const auto filteredSummary = repository.summary("chargers", {{"stationId", "1"}});
    QCOMPARE(filteredSummary.value("totalChargers"), detail.value("totalChargers"));
    QCOMPARE(filteredSummary.value("onlineChargers"), detail.value("onlineChargerCount"));
    QCOMPARE(filteredSummary.value("faultChargers").toInt(), 1);
}

void DeliveryDataContractTest::stationEmptyAndFilteredResultsUseSameContract()
{
    DatabaseConnection connection;
    QString diagnostic;
    QVERIFY2(connection.open(":memory:", true, &diagnostic), qPrintable(diagnostic));
    QSqlQuery mutation(connection.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "INSERT INTO stations(id,code,name,address,latitude,longitude,price_cents_per_kwh,status) "
        "VALUES(4,'EMPTY-4','空电站','测试地址',39,121,100,'INACTIVE')")));
    AdminRepository repository(connection.database());
    const auto empty = repository.read("stations", {{"id", "4"}}).value("item").toObject();
    QCOMPARE(empty.value("totalChargers").toInt(), 0);
    QCOMPARE(empty.value("onlineChargerCount").toInt(), 0);
    QCOMPARE(empty.value("onlineRatePercent").toDouble(), 0.0);
    const auto filtered = repository.read("stations", {{"status", "INACTIVE"}, {"pageSize", 1}});
    QCOMPARE(filtered.value("total").toInt(), 1);
    QCOMPARE(filtered.value("items").toArray().first().toObject(), empty);
    const auto stationThree = repository.read("stations", {{"id", "3"}}).value("item").toObject();
    QCOMPARE(stationThree.value("onlineRatePercent").toDouble(), 50.0);
}

void DeliveryDataContractTest::expiryUpdatesThreeTablesAndLeavesLiveReservations()
{
    DatabaseConnection connection;
    QString diagnostic;
    QVERIFY2(connection.open(":memory:", true, &diagnostic), qPrintable(diagnostic));
    QSqlQuery mutation(connection.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "INSERT INTO users(id,phone,nickname) VALUES(2,'13900000002','第二用户')")));
    ChargingRepository repository(connection.database());
    const auto start = instant("2026-09-08T01:00:00.000Z");
    const auto expired = repository.reserve(1, 1, start, start.addSecs(900), "CONTRACT-EXPIRE-1");
    QVERIFY2(expired.ok, qPrintable(expired.diagnostic));
    const auto live = repository.reserve(2, 2, start, start.addSecs(1800), "CONTRACT-EXPIRE-2");
    QVERIFY2(live.ok, qPrintable(live.diagnostic));

    // No user query or charging action follows; the worker's maintenance call
    // alone releases resources at the exact expiration boundary.
    QVERIFY2(repository.expireReservations(start.addSecs(900), &diagnostic), qPrintable(diagnostic));
    QCOMPARE(storedStatus(connection.database(), "reservations", expired.reservation.id), "EXPIRED");
    QCOMPARE(storedStatus(connection.database(), "orders", expired.order.id), "CANCELLED");
    QCOMPARE(storedStatus(connection.database(), "chargers", 1), "AVAILABLE");
    QCOMPARE(storedStatus(connection.database(), "reservations", live.reservation.id), "ACTIVE");
    QCOMPARE(storedStatus(connection.database(), "orders", live.order.id), "RESERVED");
    QCOMPARE(storedStatus(connection.database(), "chargers", 2), "RESERVED");
    QVERIFY(repository.expireReservations(start.addSecs(900))); // Repeat is safe.

    AdminRepository admin(connection.database());
    const auto charger = admin.read("chargers", {{"id", "1"}}).value("item").toObject();
    QCOMPARE(charger.value("status").toString(), QString("AVAILABLE"));
    const auto order = admin.read("orders", {{"id", QString::number(expired.order.id)}})
                           .value("item").toObject();
    QCOMPARE(order.value("status").toString(), QString("CANCELLED"));
}

void DeliveryDataContractTest::expiryRollsBackAllTablesOnFailure()
{
    DatabaseConnection connection;
    QString diagnostic;
    QVERIFY2(connection.open(":memory:", true, &diagnostic), qPrintable(diagnostic));
    ChargingRepository repository(connection.database());
    const auto start = instant("2026-09-08T01:00:00.000Z");
    const auto reserved = repository.reserve(1, 1, start, start.addSecs(900), "CONTRACT-ROLLBACK");
    QVERIFY2(reserved.ok, qPrintable(reserved.diagnostic));
    QSqlQuery mutation(connection.database());
    QVERIFY(mutation.exec(QStringLiteral(
        "CREATE TEMP TRIGGER refuse_expiry BEFORE UPDATE OF status ON orders "
        "WHEN NEW.status='CANCELLED' BEGIN SELECT RAISE(ABORT,'test rollback'); END")));
    QVERIFY(!repository.expireReservations(start.addSecs(900), &diagnostic));
    QVERIFY(!diagnostic.isEmpty());
    QCOMPARE(storedStatus(connection.database(), "reservations", reserved.reservation.id), "ACTIVE");
    QCOMPARE(storedStatus(connection.database(), "orders", reserved.order.id), "RESERVED");
    QCOMPARE(storedStatus(connection.database(), "chargers", 1), "RESERVED");
    QVERIFY(mutation.exec("DROP TRIGGER refuse_expiry"));
    QVERIFY2(repository.expireReservations(start.addSecs(900), &diagnostic), qPrintable(diagnostic));
    QVERIFY(diagnostic.isEmpty());
    QCOMPARE(storedStatus(connection.database(), "reservations", reserved.reservation.id), "EXPIRED");
    QCOMPARE(storedStatus(connection.database(), "orders", reserved.order.id), "CANCELLED");
    QCOMPARE(storedStatus(connection.database(), "chargers", 1), "AVAILABLE");
}

void DeliveryDataContractTest::expiryRejectsInvalidContext()
{
    ChargingRepository closed{QSqlDatabase()};
    QString diagnostic;
    QVERIFY(!closed.expireReservations(instant("2026-09-08T01:00:00.000Z"), &diagnostic));
    QVERIFY(!diagnostic.isEmpty());
    DatabaseConnection connection;
    QVERIFY2(connection.open(":memory:", true, &diagnostic), qPrintable(diagnostic));
    ChargingRepository repository(connection.database());
    QVERIFY(!repository.expireReservations({}, &diagnostic));
    QVERIFY(!diagnostic.isEmpty());
}

QTEST_GUILESS_MAIN(DeliveryDataContractTest)
#include "tst_delivery_data_contract.moc"
