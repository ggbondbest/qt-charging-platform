#include "user_repository.h"

#include "charging/common/model/enums.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace charging::server {

namespace {

bool readUser(const QSqlQuery& query, charging::model::User* user, QString* errorMessage)
{
    bool idIsValid = false;
    bool balanceIsValid = false;

    charging::model::User value;
    value.id = query.value(0).toLongLong(&idIsValid);
    value.phone = query.value(1).toString();
    value.nickname = query.value(2).toString();
    value.avatarKey = query.value(3).toString();
    value.balanceCents = query.value(4).toLongLong(&balanceIsValid);

    const QString statusText = query.value(5).toString();
    const QString createdAtText = query.value(6).toString();
    const QString updatedAtText = query.value(7).toString();
    value.createdAtUtc = QDateTime::fromString(createdAtText, Qt::ISODateWithMs);
    value.updatedAtUtc = QDateTime::fromString(updatedAtText, Qt::ISODateWithMs);

    if (!idIsValid || value.id <= 0 || !balanceIsValid || value.balanceCents < 0 ||
        !charging::model::fromString(statusText, &value.status) ||
        !value.createdAtUtc.isValid() || !value.updatedAtUtc.isValid()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The stored user row contains invalid data");
        }
        return false;
    }

    value.createdAtUtc = value.createdAtUtc.toUTC();
    value.updatedAtUtc = value.updatedAtUtc.toUTC();
    *user = value;
    return true;
}

} // namespace

UserRepository::UserRepository(const QSqlDatabase& database) : database_(database)
{
}

UserLookupResult UserRepository::findByPhone(const QString& phone) const
{
    UserLookupResult result;
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT id, phone, nickname, avatar_key, balance_cents, status, created_at, updated_at "
        "FROM users WHERE phone = :phone LIMIT 1"));
    query.bindValue(QStringLiteral(":phone"), phone);
    if (!query.exec()) {
        result.errorMessage = query.lastError().text();
        return result;
    }
    if (!query.next()) {
        result.ok = true;
        return result;
    }

    if (!readUser(query, &result.user, &result.errorMessage)) {
        return result;
    }
    result.ok = true;
    result.found = true;
    return result;
}

UserCreateResult UserRepository::create(const QString& phone, const QString& nickname) const
{
    UserCreateResult result;
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }

    QSqlQuery insertQuery(database_);
    insertQuery.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO users (phone, nickname, avatar_key, balance_cents, status) "
        "VALUES (:phone, :nickname, '', 0, 'ACTIVE')"));
    insertQuery.bindValue(QStringLiteral(":phone"), phone);
    insertQuery.bindValue(QStringLiteral(":nickname"), nickname);
    if (!insertQuery.exec()) {
        result.errorMessage = insertQuery.lastError().text();
        return result;
    }
    const bool inserted = insertQuery.numRowsAffected() > 0;

    const UserLookupResult lookup = findByPhone(phone);
    if (!lookup.ok) {
        result.errorMessage = lookup.errorMessage;
        return result;
    }
    if (!lookup.found) {
        result.errorMessage = QStringLiteral("User insert completed but the row was not found");
        return result;
    }

    result.ok = true;
    result.created = inserted;
    result.user = lookup.user;
    return result;
}

} // namespace charging::server
