#include "database_connection.h"
#include "order_repository.h"
#include "recharge_repository.h"
#include "user_repository.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QtTest>

class UserOrderRechargeQueriesTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void userListSupportsKeywordStatusAndPagination();
    void orderListReturnsJoinedDisplayFields();
    void rechargeIsAtomicAndIdempotent();
    void failedRechargeIsNotReportedAsIdempotentSuccess();
    void rechargeListSupportsPagination();
    void invalidQueriesFail();

private:
    qint64 userBalance() const;
    charging::server::DatabaseConnection databaseConnection_;
};

void UserOrderRechargeQueriesTest::initTestCase()
{
    QString errorMessage;
    QVERIFY2(databaseConnection_.open(QStringLiteral(":memory:"), true, &errorMessage),
             qPrintable(errorMessage));

    QSqlQuery query(databaseConnection_.database());
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO users (id, phone, nickname, status) "
        "VALUES (2, '13900139000', '冻结用户', 'FROZEN')")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO reservations "
        "(id, user_id, charger_id, status, reserved_at, expires_at, ended_at) VALUES "
        "(100, 1, 1, 'FULFILLED', '2026-09-04T08:00:00.000Z', "
        "'2026-09-04T09:00:00.000Z', '2026-09-04T08:10:00.000Z')")));
    QVERIFY(query.exec(QStringLiteral(
        "INSERT INTO orders "
        "(id, order_no, user_id, charger_id, reservation_id, status, "
        "unit_price_cents_per_kwh, energy_wh, duration_seconds, amount_cents, created_at, "
        "started_at, stopped_at, paid_at, updated_at) VALUES "
        "(100, 'ORDER-QUERY-001', 1, 1, 100, 'COMPLETED', 120, 5000, 1800, 600, "
        "'2026-09-04T08:00:00.000Z', '2026-09-04T08:10:00.000Z', "
        "'2026-09-04T08:40:00.000Z', '2026-09-04T08:41:00.000Z', "
        "'2026-09-04T08:41:00.000Z')")));
}

void UserOrderRechargeQueriesTest::cleanupTestCase()
{
    databaseConnection_.close();
}

void UserOrderRechargeQueriesTest::userListSupportsKeywordStatusAndPagination()
{
    charging::server::UserRepository repository(databaseConnection_.database());
    charging::server::UserQuery query;
    query.keyword = QStringLiteral("1390013");
    query.status = charging::model::UserStatus::Frozen;
    const auto filtered = repository.list(query);
    QVERIFY2(filtered.ok, qPrintable(filtered.errorMessage));
    QCOMPARE(filtered.totalCount, 1);
    QCOMPARE(filtered.users.first().phone, QStringLiteral("13900139000"));

    query.keyword.clear();
    query.status.reset();
    query.limit = 1;
    query.offset = 1;
    const auto page = repository.list(query);
    QVERIFY2(page.ok, qPrintable(page.errorMessage));
    QCOMPARE(page.totalCount, 2);
    QCOMPARE(page.users.size(), 1);
    QCOMPARE(page.users.first().id, qint64(2));
}

void UserOrderRechargeQueriesTest::orderListReturnsJoinedDisplayFields()
{
    charging::server::OrderRepository repository(databaseConnection_.database());
    charging::server::OrderQuery query;
    query.keyword = QStringLiteral("13800138000");
    query.status = charging::model::OrderStatus::Completed;
    const auto result = repository.list(query);
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.totalCount, 1);
    QCOMPARE(result.orders.size(), 1);
    QCOMPARE(result.orders.first().order.orderNo, QStringLiteral("ORDER-QUERY-001"));
    QCOMPARE(result.orders.first().userPhone, QStringLiteral("13800138000"));
    QCOMPARE(result.orders.first().stationName, QStringLiteral("高新园区示范充电站"));
    QCOMPARE(result.orders.first().chargerCode, QStringLiteral("CHG-DEMO-001-A1"));
}

void UserOrderRechargeQueriesTest::rechargeIsAtomicAndIdempotent()
{
    charging::server::RechargeRepository repository(databaseConnection_.database());
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-04T10:00:00.000Z"),
                                                Qt::ISODateWithMs);
    const qint64 before = userBalance();
    const auto first = repository.recharge(1, QStringLiteral("RECHARGE-QUERY-001"), 2500, now);
    QVERIFY2(first.ok, qPrintable(first.diagnostic));
    QVERIFY(!first.idempotent);
    QCOMPARE(first.record.balanceAfterCents, before + 2500);
    QCOMPARE(userBalance(), before + 2500);

    const auto repeated =
        repository.recharge(1, QStringLiteral("RECHARGE-QUERY-001"), 2500, now);
    QVERIFY2(repeated.ok, qPrintable(repeated.diagnostic));
    QVERIFY(repeated.idempotent);
    QCOMPARE(userBalance(), before + 2500);

    const auto conflicting =
        repository.recharge(1, QStringLiteral("RECHARGE-QUERY-001"), 3000, now);
    QVERIFY(!conflicting.ok);
    QCOMPARE(userBalance(), before + 2500);
}

void UserOrderRechargeQueriesTest::failedRechargeIsNotReportedAsIdempotentSuccess()
{
    const qint64 before = userBalance();
    QSqlQuery insert(databaseConnection_.database());
    insert.prepare(QStringLiteral(
        "INSERT INTO recharge_records "
        "(transaction_no, user_id, amount_cents, balance_after_cents, status, created_at) "
        "VALUES ('RECHARGE-FAILED-001', 1, 100, :balance, 'FAILED', "
        "'2026-09-04T10:10:00.000Z')"));
    insert.bindValue(QStringLiteral(":balance"), before);
    QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));

    charging::server::RechargeRepository repository(databaseConnection_.database());
    const auto result = repository.recharge(
        1, QStringLiteral("RECHARGE-FAILED-001"), 100,
        QDateTime::fromString(QStringLiteral("2026-09-04T10:11:00.000Z"), Qt::ISODateWithMs));
    QVERIFY(!result.ok);
    QVERIFY(!result.idempotent);
    QVERIFY(result.error == charging::server::RepositoryError::InvalidStateTransition);
    QCOMPARE(userBalance(), before);

    QSqlQuery cleanup(databaseConnection_.database());
    QVERIFY(cleanup.exec(QStringLiteral(
        "DELETE FROM recharge_records WHERE transaction_no = 'RECHARGE-FAILED-001'")));
}

void UserOrderRechargeQueriesTest::rechargeListSupportsPagination()
{
    charging::server::RechargeRepository repository(databaseConnection_.database());
    const auto firstPage = repository.listByUser(1, 1, 0);
    QVERIFY2(firstPage.ok, qPrintable(firstPage.errorMessage));
    QCOMPARE(firstPage.totalCount, 2);
    QCOMPARE(firstPage.records.size(), 1);
    QCOMPARE(firstPage.records.first().transactionNo, QStringLiteral("RECHARGE-QUERY-001"));

    const auto secondPage = repository.listByUser(1, 1, 1);
    QVERIFY2(secondPage.ok, qPrintable(secondPage.errorMessage));
    QCOMPARE(secondPage.totalCount, 2);
    QCOMPARE(secondPage.records.first().transactionNo, QStringLiteral("SEED-RECHARGE-0001"));
}

void UserOrderRechargeQueriesTest::invalidQueriesFail()
{
    charging::server::UserRepository userRepository(databaseConnection_.database());
    charging::server::UserQuery userQuery;
    userQuery.limit = 0;
    QVERIFY(!userRepository.list(userQuery).ok);

    charging::server::OrderRepository orderRepository(databaseConnection_.database());
    charging::server::OrderQuery orderQuery;
    orderQuery.offset = -1;
    QVERIFY(!orderRepository.list(orderQuery).ok);

    charging::server::RechargeRepository rechargeRepository(databaseConnection_.database());
    QVERIFY(!rechargeRepository.listByUser(0).ok);
    QVERIFY(!rechargeRepository.recharge(1, QStringLiteral("INVALID"), 0,
                                         QDateTime::currentDateTimeUtc())
                 .ok);
}

qint64 UserOrderRechargeQueriesTest::userBalance() const
{
    QSqlQuery query(databaseConnection_.database());
    query.prepare(QStringLiteral("SELECT balance_cents FROM users WHERE id = 1"));
    if (!query.exec() || !query.next()) {
        return -1;
    }
    return query.value(0).toLongLong();
}

QTEST_GUILESS_MAIN(UserOrderRechargeQueriesTest)

#include "tst_user_order_recharge_queries.moc"
