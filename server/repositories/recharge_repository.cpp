#include "recharge_repository.h"

#include "charging/common/model/enums.h"

#include <QSqlError>
#include <QSqlQuery>

namespace charging::server {
namespace {

constexpr int kMaximumPageSize = 100;

bool readRecord(const QSqlQuery& query, charging::model::RechargeRecord* record)
{
    bool idOk = false;
    bool userIdOk = false;
    bool amountOk = false;
    bool balanceOk = false;
    charging::model::RechargeRecord value;
    value.id = query.value(0).toLongLong(&idOk);
    value.transactionNo = query.value(1).toString();
    value.userId = query.value(2).toLongLong(&userIdOk);
    value.amountCents = query.value(3).toLongLong(&amountOk);
    value.balanceAfterCents = query.value(4).toLongLong(&balanceOk);
    value.createdAtUtc = QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs);
    if (!idOk || !userIdOk || !amountOk || !balanceOk || value.id <= 0 || value.userId <= 0 ||
        value.transactionNo.isEmpty() || value.amountCents <= 0 || value.balanceAfterCents < 0 ||
        !charging::model::fromString(query.value(5).toString(), &value.status) ||
        !value.createdAtUtc.isValid()) {
        return false;
    }
    value.createdAtUtc = value.createdAtUtc.toUTC();
    *record = value;
    return true;
}

bool loadByTransaction(const QSqlDatabase& database, const QString& transactionNo,
                       charging::model::RechargeRecord* record, bool* found, QString* diagnostic)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT id, transaction_no, user_id, amount_cents, balance_after_cents, status, "
        "created_at FROM recharge_records WHERE transaction_no = :transactionNo LIMIT 1"));
    query.bindValue(QStringLiteral(":transactionNo"), transactionNo);
    if (!query.exec()) {
        *diagnostic = query.lastError().text();
        return false;
    }
    if (!query.next()) {
        *found = false;
        return true;
    }
    if (!readRecord(query, record)) {
        *diagnostic = QStringLiteral("The stored recharge row contains invalid data");
        return false;
    }
    *found = true;
    return true;
}

} // namespace

RechargeRepository::RechargeRepository(const QSqlDatabase& database) : database_(database) {}

RechargeResult RechargeRepository::recharge(qint64 userId, const QString& transactionNo,
                                            qint64 amountCents,
                                            const QDateTime& createdAtUtc) const
{
    RechargeResult result;
    if (userId <= 0 || transactionNo.trimmed().isEmpty() || transactionNo.size() > 40 ||
        amountCents <= 0 || amountCents > charging::model::kMaximumJsonSafeInteger ||
        !createdAtUtc.isValid()) {
        result.error = RepositoryError::InvalidInput;
        return result;
    }
    if (!database_.isValid() || !database_.isOpen()) {
        result.error = RepositoryError::Database;
        result.diagnostic = QStringLiteral("The SQLite connection is not open");
        return result;
    }

    QSqlQuery begin(database_);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        result.error = RepositoryError::Database;
        result.diagnostic = begin.lastError().text();
        return result;
    }
    const auto rollback = [this]() {
        QSqlQuery query(database_);
        query.exec(QStringLiteral("ROLLBACK"));
    };

    bool found = false;
    if (!loadByTransaction(database_, transactionNo, &result.record, &found,
                           &result.diagnostic)) {
        rollback();
        result.error = RepositoryError::Database;
        return result;
    }
    if (found) {
        if (result.record.status != charging::model::RechargeStatus::Success) {
            rollback();
            result.error = RepositoryError::InvalidStateTransition;
            result.diagnostic = QStringLiteral("The previous recharge attempt failed");
            return result;
        }
        if (result.record.userId != userId || result.record.amountCents != amountCents) {
            rollback();
            result.error = RepositoryError::InvalidInput;
            return result;
        }
        QSqlQuery commit(database_);
        if (!commit.exec(QStringLiteral("COMMIT"))) {
            rollback();
            result.error = RepositoryError::Database;
            result.diagnostic = commit.lastError().text();
            return result;
        }
        result.ok = true;
        result.idempotent = true;
        return result;
    }

    const QString now = createdAtUtc.toUTC().toString(Qt::ISODateWithMs);
    QSqlQuery balanceUpdate(database_);
    balanceUpdate.prepare(QStringLiteral(
        "UPDATE users SET balance_cents = balance_cents + :amount, updated_at = :now "
        "WHERE id = :userId AND status = 'ACTIVE' "
        "AND balance_cents <= :maximumBalance"));
    balanceUpdate.bindValue(QStringLiteral(":amount"), amountCents);
    balanceUpdate.bindValue(QStringLiteral(":now"), now);
    balanceUpdate.bindValue(QStringLiteral(":userId"), userId);
    balanceUpdate.bindValue(QStringLiteral(":maximumBalance"),
                            charging::model::kMaximumJsonSafeInteger - amountCents);
    if (!balanceUpdate.exec() || balanceUpdate.numRowsAffected() != 1) {
        rollback();
        result.error = RepositoryError::InvalidStateTransition;
        result.diagnostic = balanceUpdate.lastError().text();
        return result;
    }

    QSqlQuery balanceQuery(database_);
    balanceQuery.prepare(QStringLiteral("SELECT balance_cents FROM users WHERE id = :userId"));
    balanceQuery.bindValue(QStringLiteral(":userId"), userId);
    if (!balanceQuery.exec() || !balanceQuery.next()) {
        rollback();
        result.error = RepositoryError::Database;
        result.diagnostic = balanceQuery.lastError().text();
        return result;
    }
    const qint64 balanceAfter = balanceQuery.value(0).toLongLong();

    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO recharge_records "
        "(transaction_no, user_id, amount_cents, balance_after_cents, status, created_at) "
        "VALUES (:transactionNo, :userId, :amount, :balanceAfter, 'SUCCESS', :now)"));
    insert.bindValue(QStringLiteral(":transactionNo"), transactionNo);
    insert.bindValue(QStringLiteral(":userId"), userId);
    insert.bindValue(QStringLiteral(":amount"), amountCents);
    insert.bindValue(QStringLiteral(":balanceAfter"), balanceAfter);
    insert.bindValue(QStringLiteral(":now"), now);
    if (!insert.exec()) {
        rollback();
        result.error = RepositoryError::Database;
        result.diagnostic = insert.lastError().text();
        return result;
    }
    if (!loadByTransaction(database_, transactionNo, &result.record, &found,
                           &result.diagnostic) || !found) {
        rollback();
        result.error = RepositoryError::Database;
        return result;
    }
    QSqlQuery commit(database_);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        rollback();
        result.error = RepositoryError::Database;
        result.diagnostic = commit.lastError().text();
        return result;
    }
    result.ok = true;
    return result;
}

RechargeQueryResult RechargeRepository::listByUser(qint64 userId, int limit, int offset) const
{
    RechargeQueryResult result;
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }
    if (userId <= 0 || limit <= 0 || limit > kMaximumPageSize || offset < 0) {
        result.errorMessage = QStringLiteral("Invalid recharge query parameters");
        return result;
    }

    QSqlQuery countQuery(database_);
    countQuery.prepare(
        QStringLiteral("SELECT COUNT(*) FROM recharge_records WHERE user_id = :userId"));
    countQuery.bindValue(QStringLiteral(":userId"), userId);
    if (!countQuery.exec() || !countQuery.next()) {
        result.errorMessage = countQuery.lastError().text();
        return result;
    }
    result.totalCount = countQuery.value(0).toInt();

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT id, transaction_no, user_id, amount_cents, balance_after_cents, status, "
        "created_at FROM recharge_records WHERE user_id = :userId "
        "ORDER BY created_at DESC, id DESC LIMIT :limit OFFSET :offset"));
    query.bindValue(QStringLiteral(":userId"), userId);
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);
    if (!query.exec()) {
        result.errorMessage = query.lastError().text();
        return result;
    }
    while (query.next()) {
        charging::model::RechargeRecord record;
        if (!readRecord(query, &record)) {
            result.errorMessage = QStringLiteral("The stored recharge row contains invalid data");
            result.records.clear();
            return result;
        }
        result.records.append(record);
    }
    result.ok = true;
    return result;
}

} // namespace charging::server
