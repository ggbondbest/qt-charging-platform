#pragma once

#include "charging/common/model/models.h"

#include <QSqlDatabase>
#include <QString>

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

class UserRepository final
{
public:
    explicit UserRepository(const QSqlDatabase& database);

    UserLookupResult findByPhone(const QString& phone) const;

    // Uses INSERT OR IGNORE followed by a lookup. This makes concurrent first
    // login safe when another connection inserts the same unique phone first.
    UserCreateResult create(const QString& phone, const QString& nickname) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
