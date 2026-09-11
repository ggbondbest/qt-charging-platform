#include "database_connection.h"
#include "operation_log_repository.h"
#include "reservation_query_repository.h"

#include <QSqlQuery>
#include <QtTest>

class ReservationOperationLogsTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void reservationQueryReturnsDisplayContext();
    void reservationQueryEnforcesUserAndStatusFilters();
    void operationLogRoundTripsJsonAndFilters();
    void operationLogRejectsMissingAdmin();
    void invalidQueriesFail();

private:
    charging::server::DatabaseConnection databaseConnection_;
};

void ReservationOperationLogsTest::initTestCase()
{
    QString errorMessage;
    QVERIFY2(databaseConnection_.open(QStringLiteral(":memory:"), true, &errorMessage),
             qPrintable(errorMessage));
    QSqlQuery query(databaseConnection_.database());
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO reservations "
        "(id, user_id, charger_id, status, reserved_at, expires_at, ended_at) VALUES "
        "(200, 1, 1, 'FULFILLED', '2026-09-04T08:00:00.000Z', "
        "'2026-09-04T09:00:00.000Z', '2026-09-04T08:30:00.000Z')")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO orders "
        "(id, order_no, user_id, charger_id, reservation_id, status, "
        "unit_price_cents_per_kwh, created_at, updated_at) VALUES "
        "(200, 'ORDER-RESERVATION-200', 1, 1, 200, 'CANCELLED', 120, "
        "'2026-09-04T08:00:00.000Z', '2026-09-04T08:30:00.000Z')")));
}

void ReservationOperationLogsTest::cleanupTestCase()
{
    databaseConnection_.close();
}

void ReservationOperationLogsTest::reservationQueryReturnsDisplayContext()
{
    charging::server::ReservationQueryRepository repository(databaseConnection_.database());
    const auto result = repository.listByUser(1);
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.totalCount, 1);
    QCOMPARE(result.reservations.size(), 1);
    const auto& item = result.reservations.first();
    QCOMPARE(item.reservation.id, qint64(200));
    QCOMPARE(item.orderNo, QStringLiteral("ORDER-RESERVATION-200"));
    QCOMPARE(item.stationName, QStringLiteral("高新园区示范充电站"));
    QCOMPARE(item.chargerCode, QStringLiteral("CHG-DEMO-001-A1"));
    QCOMPARE(item.chargerPowerWatts, 120000);
    QCOMPARE(item.priceCentsPerKwh, qint64(120));
    QVERIFY(item.chargerType == charging::model::ChargerType::Fast);
}

void ReservationOperationLogsTest::reservationQueryEnforcesUserAndStatusFilters()
{
    charging::server::ReservationQueryRepository repository(databaseConnection_.database());
    const auto matched =
        repository.listByUser(1, charging::model::ReservationStatus::Fulfilled, 1, 0);
    QVERIFY2(matched.ok, qPrintable(matched.errorMessage));
    QCOMPARE(matched.totalCount, 1);
    QCOMPARE(matched.reservations.size(), 1);

    const auto wrongStatus =
        repository.listByUser(1, charging::model::ReservationStatus::Active);
    QVERIFY2(wrongStatus.ok, qPrintable(wrongStatus.errorMessage));
    QCOMPARE(wrongStatus.totalCount, 0);

    const auto anotherUser = repository.listByUser(2);
    QVERIFY2(anotherUser.ok, qPrintable(anotherUser.errorMessage));
    QCOMPARE(anotherUser.totalCount, 0);
}

void ReservationOperationLogsTest::operationLogRoundTripsJsonAndFilters()
{
    charging::server::OperationLogRepository repository(databaseConnection_.database());
    QJsonObject details;
    details.insert(QStringLiteral("previousStatus"), QStringLiteral("FAULT"));
    details.insert(QStringLiteral("newStatus"), QStringLiteral("AVAILABLE"));
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-04T11:00:00.000Z"),
                                                Qt::ISODateWithMs);
    const auto appended = repository.append(1, QStringLiteral("CLEAR_CHARGER_ALERT"),
                                            QStringLiteral("CHARGER"), QStringLiteral("3"),
                                            details, now);
    QVERIFY2(appended.ok, qPrintable(appended.errorMessage));
    QCOMPARE(appended.log.adminId, qint64(1));
    QCOMPARE(appended.log.details.value(QStringLiteral("newStatus")).toString(),
             QStringLiteral("AVAILABLE"));

    const auto listed = repository.list(1, QStringLiteral("CLEAR"), 1, 0);
    QVERIFY2(listed.ok, qPrintable(listed.errorMessage));
    QCOMPARE(listed.totalCount, 1);
    QCOMPARE(listed.logs.size(), 1);
    QCOMPARE(listed.logs.first().targetType, QStringLiteral("CHARGER"));
}

void ReservationOperationLogsTest::operationLogRejectsMissingAdmin()
{
    charging::server::OperationLogRepository repository(databaseConnection_.database());
    const auto result = repository.append(
        999, QStringLiteral("UPDATE_STATION"), QStringLiteral("STATION"), QStringLiteral("1"),
        {}, QDateTime::currentDateTimeUtc());
    QVERIFY(!result.ok);

    const auto listed = repository.list();
    QVERIFY2(listed.ok, qPrintable(listed.errorMessage));
    QCOMPARE(listed.totalCount, 1);
}

void ReservationOperationLogsTest::invalidQueriesFail()
{
    charging::server::ReservationQueryRepository reservationRepository(
        databaseConnection_.database());
    QVERIFY(!reservationRepository.listByUser(0).ok);
    QVERIFY(!reservationRepository.listByUser(1, std::nullopt, 101).ok);

    charging::server::OperationLogRepository logRepository(databaseConnection_.database());
    QVERIFY(!logRepository.list(-1).ok);
    QVERIFY(!logRepository.append(1, {}, QStringLiteral("CHARGER"), {}, {},
                                  QDateTime::currentDateTimeUtc())
                 .ok);
}

QTEST_GUILESS_MAIN(ReservationOperationLogsTest)

#include "tst_reservation_operation_logs.moc"
