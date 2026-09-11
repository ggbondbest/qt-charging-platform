#include "database_connection.h"
#include "server_runtime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QtTest>

#include <future>

using charging::server::DatabaseConnection;
using charging::server::ServerRuntime;

namespace {

QVariant scalar(const DatabaseConnection& connection, const QString& sql)
{
    QSqlQuery query(connection.database());
    if (!query.exec(sql) || !query.next()) {
        QTest::qFail(qPrintable(query.lastError().text()), __FILE__, __LINE__);
        return {};
    }
    return query.value(0);
}

QByteArray snapshot(const DatabaseConnection& connection)
{
    QJsonObject tables;
    for (const QString& table : {QStringLiteral("admins"), QStringLiteral("users"),
                                QStringLiteral("recharge_records"), QStringLiteral("orders"),
                                QStringLiteral("reservations"), QStringLiteral("stations"),
                                QStringLiteral("chargers")}) {
        QSqlQuery query(connection.database());
        if (!query.exec(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table))) {
            QTest::qFail(qPrintable(query.lastError().text()), __FILE__, __LINE__);
            return {};
        }
        QJsonArray rows;
        while (query.next()) {
            QJsonArray row;
            for (int column = 0; column < query.record().count(); ++column) {
                row.append(QJsonValue::fromVariant(query.value(column)));
            }
            rows.append(row);
        }
        tables.insert(table, rows);
    }
    return QJsonDocument(tables).toJson(QJsonDocument::Compact);
}

} // namespace

class CityDemoSeedTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesSmallRepositoryFixture();
    void fiveCitiesHaveFiveStationsAndThreeChargers();
    void repeatedApplicationPreservesEditsAndBusinessState();
    void usesNaturalCodesAndNeverFixedNumericIds();
    void failedApplicationRollsBackAllNewRows();
    void concurrentApplicationsRemainIdempotent();
    void runtimeLoadsCatalogOnlyWithDemoSeed();
    void rejectsClosedConnection();
};

void CityDemoSeedTest::preservesSmallRepositoryFixture()
{
    DatabaseConnection connection;
    QString error;
    QVERIFY2(connection.open(":memory:", true, &error), qPrintable(error));
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM stations").toInt(), 3);
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM chargers").toInt(), 7);
}

void CityDemoSeedTest::fiveCitiesHaveFiveStationsAndThreeChargers()
{
    DatabaseConnection connection;
    QString error;
    // Catalog can also be applied to a schema-only DB; no seeded user is needed.
    QVERIFY2(connection.open(":memory:", false, &error), qPrintable(error));
    QVERIFY2(connection.applyCityDemoSeed(&error), qPrintable(error));
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM stations").toInt(), 25);
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM chargers").toInt(), 75);
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM users").toInt(), 0);
    for (const QString& city : {QStringLiteral("大连市"), QStringLiteral("沈阳市"),
                                QStringLiteral("北京市"), QStringLiteral("上海市"),
                                QStringLiteral("深圳市")}) {
        QCOMPARE(scalar(connection, QStringLiteral("SELECT COUNT(*) FROM stations "
            "WHERE address LIKE '%1%'").arg(city)).toInt(), 5);
    }
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM (SELECT station_id FROM chargers "
        "GROUP BY station_id HAVING COUNT(*)=3 AND SUM(type='FAST')=2 AND SUM(type='SLOW')=1)")
        .toInt(), 25);
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM stations WHERE "
        "name NOT LIKE '%示范%' OR latitude=0 OR longitude=0").toInt(), 0);
    QCOMPARE(scalar(connection, "PRAGMA integrity_check").toString(), QString("ok"));
    QSqlQuery foreignKeys(connection.database());
    QVERIFY(foreignKeys.exec("PRAGMA foreign_key_check"));
    QVERIFY(!foreignKeys.next());
}

void CityDemoSeedTest::repeatedApplicationPreservesEditsAndBusinessState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("existing-demo.sqlite");
    DatabaseConnection connection;
    QString error;
    QVERIFY2(connection.open(path, true, &error), qPrintable(error));
    QVERIFY2(connection.applyCityDemoSeed(&error), qPrintable(error));
    {
        QSqlQuery query(connection.database());
        for (const auto* sql : {
             "UPDATE users SET balance_cents=87654,nickname='existing-user',status='FROZEN' WHERE id=1",
             "UPDATE stations SET name='operator-edit',address='edited-address',status='INACTIVE',"
             "price_cents_per_kwh=999,latitude=38.91,longitude=121.63 WHERE code='STA-CITY-DL-004'",
             "UPDATE chargers SET status='FAULT',total_charge_count=123,total_charge_seconds=456789 "
             "WHERE code='CHG-CITY-DL-004-A1'",
             "UPDATE chargers SET status='RESERVED' WHERE id=1",
             "INSERT INTO reservations(id,user_id,charger_id,status,reserved_at,expires_at) "
             "VALUES(1,1,1,'ACTIVE','2026-09-09T00:00:00.000Z','2099-09-09T00:00:00.000Z')",
             "INSERT INTO orders(order_no,user_id,charger_id,reservation_id,status,unit_price_cents_per_kwh) "
             "VALUES('KEEP-ORDER-001',1,1,1,'RESERVED',120)"}) {
            QVERIFY2(query.exec(QString::fromLatin1(sql)), qPrintable(query.lastError().text()));
        }
    }
    const auto before = snapshot(connection);
    QVERIFY2(connection.applyCityDemoSeed(&error), qPrintable(error));
    QCOMPARE(snapshot(connection), before);
    connection.close();
    // Normal --demo-seed reopening still must not reset balances or reservations.
    QVERIFY2(connection.open(path, true, &error), qPrintable(error));
    QVERIFY2(connection.applyCityDemoSeed(&error), qPrintable(error));
    QCOMPARE(snapshot(connection), before);
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM stations").toInt(), 25);
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM chargers").toInt(), 75);
}

void CityDemoSeedTest::usesNaturalCodesAndNeverFixedNumericIds()
{
    DatabaseConnection connection;
    QString error;
    QVERIFY2(connection.open(":memory:", false, &error), qPrintable(error));
    {
        QSqlQuery query(connection.database());
        QVERIFY(query.exec("INSERT INTO stations(id,code,name,address,latitude,longitude,price_cents_per_kwh) "
                           "VALUES(5000,'CUSTOM-5000','custom-station','custom-address',1,2,99)"));
        QVERIFY(query.exec("INSERT INTO chargers(id,station_id,code,type,power_watts,status) "
                           "VALUES(8000,5000,'CUSTOM-8000','SLOW',11000,'OFFLINE')"));
    }
    QVERIFY2(connection.applyCityDemoSeed(&error), qPrintable(error));
    QCOMPARE(scalar(connection, "SELECT name FROM stations WHERE id=5000").toString(),
             QString("custom-station"));
    QCOMPARE(scalar(connection, "SELECT status FROM chargers WHERE id=8000").toString(),
             QString("OFFLINE"));
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM stations WHERE id>5000").toInt(), 25);
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM chargers WHERE station_id>5000").toInt(), 75);
}

void CityDemoSeedTest::failedApplicationRollsBackAllNewRows()
{
    DatabaseConnection connection;
    QString error;
    QVERIFY2(connection.open(":memory:", true, &error), qPrintable(error));
    const auto before = snapshot(connection);
    {
        QSqlQuery query(connection.database());
        QVERIFY(query.exec("CREATE TEMP TRIGGER reject_city BEFORE INSERT ON stations "
                           "WHEN NEW.code='STA-CITY-SY-002' BEGIN "
                           "SELECT RAISE(ABORT,'test-injected-city-failure'); END"));
    }
    QVERIFY(!connection.applyCityDemoSeed(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(snapshot(connection), before);
    {
        QSqlQuery query(connection.database());
        QVERIFY(query.exec("DROP TRIGGER reject_city"));
    }
    QVERIFY2(connection.applyCityDemoSeed(&error), qPrintable(error));
    QCOMPARE(scalar(connection, "SELECT COUNT(*) FROM stations").toInt(), 25);
}

void CityDemoSeedTest::concurrentApplicationsRemainIdempotent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("concurrent-demo.sqlite");
    DatabaseConnection setup;
    QString error;
    QVERIFY2(setup.open(path, true, &error), qPrintable(error));
    setup.close();

    std::promise<void> start;
    const auto gate = start.get_future().share();
    const auto load = [path, gate] {
        gate.wait();
        DatabaseConnection connection;
        QString failure;
        if (!connection.open(path, false, &failure) ||
            !connection.applyCityDemoSeed(&failure)) {
            return failure;
        }
        return QString();
    };
    auto first = std::async(std::launch::async, load);
    auto second = std::async(std::launch::async, load);
    start.set_value();
    const auto firstFailure = first.get();
    const auto secondFailure = second.get();
    QVERIFY2(firstFailure.isEmpty(), qPrintable(firstFailure));
    QVERIFY2(secondFailure.isEmpty(), qPrintable(secondFailure));
    QVERIFY2(setup.open(path, false, &error), qPrintable(error));
    QCOMPARE(scalar(setup, "SELECT COUNT(*) FROM stations").toInt(), 25);
    QCOMPARE(scalar(setup, "SELECT COUNT(*) FROM chargers").toInt(), 75);
}

void CityDemoSeedTest::runtimeLoadsCatalogOnlyWithDemoSeed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("runtime-demo.sqlite");
    bool catalogPreviouslyLoaded = false;
    for (const bool withDemoSeed : {false, true, false}) {
        ServerRuntime runtime;
        QSignalSpy listening(&runtime, &ServerRuntime::listening);
        QSignalSpy failed(&runtime, &ServerRuntime::startupFailed);
        QSignalSpy stopped(&runtime, &ServerRuntime::stopped);
        QVERIFY(runtime.start(path, withDemoSeed, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(listening.count(), 1);
        QCOMPARE(failed.count(), 0);
        DatabaseConnection observer;
        QString error;
        QVERIFY2(observer.open(path, false, &error), qPrintable(error));
        const auto stations = scalar(observer, "SELECT COUNT(*) FROM stations").toInt();
        if (withDemoSeed) {
            catalogPreviouslyLoaded = true;
            QCOMPARE(stations, 25);
            QCOMPARE(scalar(observer, "SELECT COUNT(*) FROM chargers").toInt(), 75);
        } else {
            // No implicit demo data for a fresh non-demo DB; restarting an
            // expanded demo DB without the flag keeps its existing catalog.
            QCOMPARE(stations, catalogPreviouslyLoaded ? 25 : 0);
        }
        observer.close();
        runtime.stop();
        QTRY_COMPARE(stopped.count(), 1);
    }
}

void CityDemoSeedTest::rejectsClosedConnection()
{
    DatabaseConnection connection;
    QString error;
    QVERIFY(!connection.applyCityDemoSeed(&error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(CityDemoSeedTest)
#include "tst_city_demo_seed.moc"
