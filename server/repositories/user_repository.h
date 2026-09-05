#pragma once

#include "charging/common/model/models.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <optional>

namespace charging::server {

struct UserLookupResult
{
    bool ok = false;
    bool found = false;
    charging::model::User user;
    QString errorMessage;
};

struct UserCreateResult
{
    bool ok = false;
    bool created = false;
    charging::model::User user;
    QString errorMessage;
};

struct UserQuery
{
    QString keyword;
    std::optional<charging::model::UserStatus> status;
    int limit = 20;
    int offset = 0;
};

struct UserQueryResult
{
    bool ok = false;
    QVector<charging::model::User> users;
    int totalCount = 0;
    QString errorMessage;
};

class UserRepository final
{
public:
    explicit UserRepository(const QSqlDatabase& database);

    UserLookupResult findByPhone(const QString& phone) const;
    UserQueryResult list(const UserQuery& query) const;

    // Uses INSERT OR IGNORE followed by a lookup. This makes concurrent first
    // login safe when another connection inserts the same unique phone first.
    UserCreateResult create(const QString& phone, const QString& nickname) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
