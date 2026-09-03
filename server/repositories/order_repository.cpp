#include "order_repository.h"

#include "charging/common/model/models.h"
#include "repository_row_mapper.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace charging::server {

namespace {

const QString kOrderColumns =
    QStringLiteral("id, order_no, user_id, charger_id, reservation_id, status, "
                   "unit_price_cents_per_kwh, energy_wh, duration_seconds, amount_cents, "
                   "created_at, started_at, stopped_at, paid_at, updated_at");

QString toStorageUtc(const QDateTime& value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

OrderRepositoryResult failure(RepositoryError error, const QString& diagnostic = {})
{
    OrderRepositoryResult result;
    result.error = error;
    result.diagnostic = diagnostic;
    return result;
}

class ImmediateTransaction final
{
public:
    explicit ImmediateTransaction(const QSqlDatabase& database) : database_(database) {}

    ~ImmediateTransaction()
    {
        if (active_) {
            QSqlQuery rollback(database_);
            rollback.exec(QStringLiteral("ROLLBACK"));
        }
    }

    bool begin(QString* diagnostic)
    {
        QSqlQuery query(database_);
        if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
            *diagnostic = query.lastError().text();
            return false;
        }
        active_ = true;
        return true;
    }

    bool commit(QString* diagnostic)
    {
        QSqlQuery query(database_);
        if (!query.exec(QStringLiteral("COMMIT"))) {
            *diagnostic = query.lastError().text();
            return false;
        }
        active_ = false;
        return true;
    }

private:
    QSqlDatabase database_;
    bool active_ = false;
};

bool loadOrder(const QSqlDatabase& database, qint64 orderId, charging::model::Order* order,
               bool* found, QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("SELECT %1 FROM orders WHERE id = :id LIMIT 1").arg(kOrderColumns));
    query.bindValue(QStringLiteral(":id"), orderId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *found = false;
        return true;
    }
    if (!repository_detail::readOrder(query, order)) {
        *diagnostic = QStringLiteral("The stored order row contains invalid data");
        return false;
    }
    *found = true;
    return true;
}

bool loadBalance(const QSqlDatabase& database, qint64 userId, qint64* balance, bool* found,
                 QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT balance_cents FROM users WHERE id = :id LIMIT 1"));
    query.bindValue(QStringLiteral(":id"), userId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *found = false;
        return true;
    }
    bool balanceOk = false;
    const qint64 value = query.value(0).toLongLong(&balanceOk);
    if (!balanceOk || !repository_detail::inSafeRange(value)) {
        *diagnostic = QStringLiteral("The stored user balance is invalid");
        return false;
    }
    *balance = value;
    *found = true;
    return true;
}

bool reservationMatchesPayableOrder(const QSqlDatabase& database,
                                    const charging::model::Order& order, QString* diagnostic)
{
    if (order.reservationId <= 0) {
        *diagnostic = QStringLiteral("The payable order has no reservation");
        return false;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT user_id, charger_id, status FROM reservations WHERE id = :id LIMIT 1"));
    query.bindValue(QStringLiteral(":id"), order.reservationId);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *diagnostic = QStringLiteral("The payable order references a missing reservation");
        return false;
    }

    bool userIdOk = false;
    bool chargerIdOk = false;
    const qint64 reservationUserId = query.value(0).toLongLong(&userIdOk);
    const qint64 reservationChargerId = query.value(1).toLongLong(&chargerIdOk);
    if (!userIdOk || !chargerIdOk || reservationUserId != order.userId ||
        reservationChargerId != order.chargerId ||
        query.value(2).toString() != QStringLiteral("FULFILLED")) {
        *diagnostic = QStringLiteral("The payable order and reservation have inconsistent state");
        return false;
    }
    return true;
}

} // namespace

OrderRepository::OrderRepository(const QSqlDatabase& database) : database_(database) {}

OrderRepositoryResult OrderRepository::pay(qint64 userId, qint64 orderId,
                                           const QDateTime& paidAtUtc) const
{
    if (userId <= 0 || orderId <= 0 || !paidAtUtc.isValid()) {
        return failure(RepositoryError::InvalidInput);
    }
    if (!database_.isValid() || !database_.isOpen()) {
        return failure(RepositoryError::Database,
                       QStringLiteral("The SQLite connection is not open"));
    }

    OrderRepositoryResult result;
    ImmediateTransaction transaction(database_);
    if (!transaction.begin(&result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }

    bool found = false;
    if (!loadOrder(database_, orderId, &result.order, &found, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (!found) {
        return failure(RepositoryError::NotFound);
    }
    if (result.order.userId != userId) {
        return failure(RepositoryError::Unauthorized);
    }

    if (result.order.status == charging::model::OrderStatus::Completed) {
        if (!reservationMatchesPayableOrder(database_, result.order, &result.diagnostic)) {
            return failure(RepositoryError::Database, result.diagnostic);
        }
        if (!loadBalance(database_, userId, &result.balanceCents, &found, &result.diagnostic)) {
            return failure(RepositoryError::Database, result.diagnostic);
        }
        if (!found) {
            return failure(RepositoryError::NotFound);
        }
        if (!transaction.commit(&result.diagnostic)) {
            return failure(RepositoryError::Database, result.diagnostic);
        }
        result.ok = true;
        result.idempotent = true;
        return result;
    }
    if (result.order.status != charging::model::OrderStatus::WaitingPayment) {
        return failure(RepositoryError::InvalidStateTransition);
    }
    if (!reservationMatchesPayableOrder(database_, result.order, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (!result.order.stoppedAtUtc.isValid() || paidAtUtc.toUTC() < result.order.stoppedAtUtc) {
        return failure(RepositoryError::InvalidStateTransition);
    }

    qint64 currentBalance = 0;
    if (!loadBalance(database_, userId, &currentBalance, &found, &result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    if (!found) {
        return failure(RepositoryError::NotFound);
    }
    if (currentBalance < result.order.amountCents) {
        return failure(RepositoryError::InsufficientBalance);
    }

    const QString now = toStorageUtc(paidAtUtc);
    QSqlQuery balanceUpdate(database_);
    balanceUpdate.prepare(QStringLiteral(
        "UPDATE users SET balance_cents = balance_cents - :amount, updated_at = :now "
        "WHERE id = :userId AND balance_cents >= :amount"));
    balanceUpdate.bindValue(QStringLiteral(":amount"), result.order.amountCents);
    balanceUpdate.bindValue(QStringLiteral(":now"), now);
    balanceUpdate.bindValue(QStringLiteral(":userId"), userId);
    if (!balanceUpdate.exec()) {
        return failure(RepositoryError::Database, balanceUpdate.lastError().text());
    }
    if (balanceUpdate.numRowsAffected() != 1) {
        // The conditional UPDATE is the authority. A zero-row result performs
        // no debit and the order remains WAITING_PAYMENT.
        return failure(RepositoryError::InsufficientBalance);
    }

    QSqlQuery orderUpdate(database_);
    orderUpdate.prepare(
        QStringLiteral("UPDATE orders SET status = 'COMPLETED', paid_at = :now, updated_at = :now "
                       "WHERE id = :orderId AND user_id = :userId AND status = 'WAITING_PAYMENT'"));
    orderUpdate.bindValue(QStringLiteral(":now"), now);
    orderUpdate.bindValue(QStringLiteral(":orderId"), orderId);
    orderUpdate.bindValue(QStringLiteral(":userId"), userId);
    if (!orderUpdate.exec()) {
        return failure(RepositoryError::Database, orderUpdate.lastError().text());
    }
    if (orderUpdate.numRowsAffected() != 1) {
        return failure(RepositoryError::Database,
                       QStringLiteral("The conditional payment update affected no row"));
    }

    if (!loadOrder(database_, orderId, &result.order, &found, &result.diagnostic) || !found ||
        !loadBalance(database_, userId, &result.balanceCents, &found, &result.diagnostic) ||
        !found) {
        if (result.diagnostic.isEmpty()) {
            result.diagnostic = QStringLiteral("Payment result rows could not be reloaded");
        }
        return failure(RepositoryError::Database, result.diagnostic);
    }

    if (!transaction.commit(&result.diagnostic)) {
        return failure(RepositoryError::Database, result.diagnostic);
    }
    result.ok = true;
    result.error = RepositoryError::None;
    return result;
}

} // namespace charging::server
