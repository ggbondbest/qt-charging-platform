#include "dashboard_repository.h"
#include "database_connection.h"

#include <QSqlQuery>
#include <QtTest>

class DashboardRepositoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void summaryUsesConsistentBusinessDefinitions();
    void revenueTrendFillsDatesWithoutOrders();
    void invalidInputsFail();

private:
    charging::server::DatabaseConnection databaseConnection_;
};

void DashboardRepositoryTest::initTestCase()
{
    QString errorMessage;
    QVERIFY2(databaseConnection_.open(QStringLiteral(":memory:"), true, &errorMessage),
             qPrintable(errorMessage));
    QSqlQuery query(databaseConnection_.database());
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO reservations "
        "(id, user_id, charger_id, status, reserved_at, expires_at, ended_at) VALUES "
        "(301, 1, 1, 'FULFILLED', '2026-09-04T07:00:00.000Z', "
        "'2026-09-04T08:00:00.000Z', '2026-09-04T07:30:00.000Z'), "
        "(302, 1, 2, 'FULFILLED', '2026-09-02T07:00:00.000Z', "
        "'2026-09-02T08:00:00.000Z', '2026-09-02T07:30:00.000Z')")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO orders "
        "(id, order_no, user_id, charger_id, reservation_id, status, "
        "unit_price_cents_per_kwh, energy_wh, duration_seconds, amount_cents, created_at, "
        "started_at, stopped_at, paid_at, updated_at) VALUES "
        "(301, 'DASHBOARD-301', 1, 1, 301, 'COMPLETED', 120, 5000, 1800, 600, "
        "'2026-09-04T07:00:00.000Z', '2026-09-04T07:05:00.000Z', "
        "'2026-09-04T07:35:00.000Z', '2026-09-04T07:36:00.000Z', "
        "'2026-09-04T07:36:00.000Z'), "
        "(302, 'DASHBOARD-302', 1, 2, 302, 'COMPLETED', 120, 3000, 1200, 400, "
        "'2026-09-02T07:00:00.000Z', '2026-09-02T07:05:00.000Z', "
        "'2026-09-02T07:25:00.000Z', '2026-09-02T07:26:00.000Z', "
        "'2026-09-02T07:26:00.000Z')")));
}

void DashboardRepositoryTest::cleanupTestCase()
{
    databaseConnection_.close();
}

void DashboardRepositoryTest::summaryUsesConsistentBusinessDefinitions()
{
    charging::server::DashboardRepository repository(databaseConnection_.database());
    const QDateTime observedAt = QDateTime::fromString(
        QStringLiteral("2026-09-04T12:00:00.000Z"), Qt::ISODateWithMs);
    const auto result = repository.summary(observedAt);
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.summary.totalUsers, 1);
    QCOMPARE(result.summary.activeStations, 3);
    QCOMPARE(result.summary.totalChargers, 7);
    QCOMPARE(result.summary.availableChargers, 5);
    QCOMPARE(result.summary.reservedChargers, 0);
    QCOMPARE(result.summary.chargingChargers, 0);
    QCOMPARE(result.summary.faultChargers, 1);
    QCOMPARE(result.summary.offlineChargers, 1);
    QCOMPARE(result.summary.activeOrders, 0);
    QCOMPARE(result.summary.todayRevenueCents, qint64(600));
    QCOMPARE(result.summary.monthRevenueCents, qint64(1000));
}

void DashboardRepositoryTest::revenueTrendFillsDatesWithoutOrders()
{
    charging::server::DashboardRepository repository(databaseConnection_.database());
    const auto result = repository.revenueTrend(QDate(2026, 9, 1), QDate(2026, 9, 4));
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.points.size(), 4);
    QCOMPARE(result.points.at(0).date, QDate(2026, 9, 1));
    QCOMPARE(result.points.at(0).revenueCents, qint64(0));
    QCOMPARE(result.points.at(1).completedOrderCount, 1);
    QCOMPARE(result.points.at(1).revenueCents, qint64(400));
    QCOMPARE(result.points.at(2).revenueCents, qint64(0));
    QCOMPARE(result.points.at(3).completedOrderCount, 1);
    QCOMPARE(result.points.at(3).revenueCents, qint64(600));
}

void DashboardRepositoryTest::invalidInputsFail()
{
    charging::server::DashboardRepository repository(databaseConnection_.database());
    QVERIFY(!repository.summary({}).ok);
    QVERIFY(!repository.revenueTrend(QDate(2026, 9, 4), QDate(2026, 9, 1)).ok);
    QVERIFY(!repository.revenueTrend(QDate(2026, 8, 1), QDate(2026, 9, 4)).ok);
}

QTEST_APPLESS_MAIN(DashboardRepositoryTest)

#include "tst_dashboard_repository.moc"
