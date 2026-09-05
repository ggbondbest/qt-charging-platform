#pragma once

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QVariantMap>

namespace charging::server {

// Application-specific queries, separate from the member-5 admin repositories.
// Rows use SQL column names; only the Service knows the wire representation.
enum class UserApiAction { Stations, Chargers, Reservations, Profile, UpdateProfile,
                           Recharge, RechargeRecords, Orders };
enum class UserApiError { None, Database, Unauthorized, Frozen, NotFound, Invalid,
                          Conflict, RechargeFailed, TooManyRows };
struct UserApiQuery {
    UserApiAction action = UserApiAction::Profile;
    qint64 userId = 0;
    qint64 stationId = 0;
    int page = 1;
    int pageSize = 20;
    QString keyword;
    QString status;
    bool updateNickname = false;
    bool updateAvatar = false;
    QString nickname;
    QString avatarKey;
    QString transactionNo;
    qint64 amountCents = 0;
    QDateTime nowUtc;
};
struct UserApiResult {
    UserApiError error = UserApiError::Database;
    QList<QVariantMap> rows;
    int total = 0;
    qint64 balanceCents = 0;
    bool idempotent = false;
};

class UserApiRepository final {
public:
    explicit UserApiRepository(const QSqlDatabase& database);
    // User state, count/list and writes share a transaction. Recharge balance
    // and SUCCESS record commit together under SQLite's writer lock.
    UserApiResult execute(const UserApiQuery& input) const;
private:
    QSqlDatabase database_;
};

} // namespace charging::server
