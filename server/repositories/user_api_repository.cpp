#include "user_api_repository.h"
#include "charging_repository.h"
#include "charging/common/model/models.h"
#include "charging/common/protocol/user_api_contract.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <limits>

namespace charging::server {
namespace {
class Transaction final {
public:
    explicit Transaction(QSqlDatabase db) : db_(db) {}
    ~Transaction() { if (active_) db_.rollback(); }
    bool begin() { QSqlQuery q(db_); active_ = q.exec("BEGIN IMMEDIATE"); return active_; }
    bool commit() { if (!db_.commit()) return false; active_ = false; return true; }
private:
    QSqlDatabase db_;
    bool active_ = false;
};

QVariantMap row(const QSqlQuery& query)
{
    QVariantMap result;
    const QSqlRecord record = query.record();
    for (int i = 0; i < record.count(); ++i) result.insert(record.fieldName(i), query.value(i));
    return result;
}
bool run(QSqlQuery& query, const QString& sql, const QVariantMap& values = {})
{
    if (!query.prepare(sql)) return false;
    for (auto it = values.begin(); it != values.end(); ++it) query.bindValue(":" + it.key(), it.value());
    return query.exec();
}
UserApiResult failure(UserApiError error) { UserApiResult r; r.error = error; return r; }
} // namespace

UserApiRepository::UserApiRepository(const QSqlDatabase& database) : database_(database) {}

UserApiResult UserApiRepository::execute(const UserApiQuery& in) const
{
    if (in.userId <= 0) return failure(UserApiError::Unauthorized);
    if (in.page < 1 || in.pageSize < 1 || in.pageSize > 100 || !in.nowUtc.isValid())
        return failure(UserApiError::Invalid);
    Transaction tx(database_);
    if (!tx.begin()) return {};
    QSqlQuery q(database_);
    if (!run(q, "SELECT * FROM users WHERE id=:uid", {{"uid", in.userId}})) return {};
    if (!q.next()) return failure(UserApiError::Unauthorized);
    QVariantMap user = row(q);
    q.finish();
    if (user.value("status").toString() != "ACTIVE") return failure(UserApiError::Frozen);
    UserApiResult result;
    const QString now = in.nowUtc.toUTC().toString(Qt::ISODateWithMs);
    const auto finish = [&]() {
        if (!tx.commit()) return UserApiResult{};
        result.error = UserApiError::None;
        return result;
    };
    if (in.action == UserApiAction::Profile) {
        result.rows.append(user);
        return finish();
    }
    if (in.action == UserApiAction::UpdateProfile) {
        if ((!in.updateNickname && !in.updateAvatar)
            || (in.updateNickname && (in.nickname.trimmed().isEmpty() || in.nickname.size() > 32)))
            return failure(UserApiError::Invalid);
        if (!run(q, "UPDATE users SET nickname=:nick, avatar_key=:avatar, updated_at=:now WHERE id=:uid",
                 {{"nick", in.updateNickname ? in.nickname : user.value("nickname")},
                  {"avatar", in.updateAvatar ? in.avatarKey : user.value("avatar_key")},
                  {"now", now}, {"uid", in.userId}}) || q.numRowsAffected() != 1) return {};
        if (!run(q, "SELECT * FROM users WHERE id=:uid", {{"uid", in.userId}}) || !q.next()) return {};
        result.rows.append(row(q));
        q.finish();
        return finish();
    }
    if (in.action == UserApiAction::Recharge) {
        if (in.amountCents < 1 || in.amountCents > charging::protocol::user_api::kMaximumRechargeCents
            || in.transactionNo.isEmpty() || in.transactionNo.size() > 40)
            return failure(UserApiError::Invalid);
        if (!run(q, "SELECT * FROM recharge_records WHERE transaction_no=:txn",
                 {{"txn", in.transactionNo}})) return {};
        if (q.next()) {
            const QVariantMap record = row(q);
            q.finish();
            if (record.value("user_id").toLongLong() != in.userId
                || record.value("amount_cents").toLongLong() != in.amountCents)
                return failure(UserApiError::Conflict);
            if (record.value("status").toString() != "SUCCESS")
                return failure(UserApiError::RechargeFailed);
            result.rows.append(record);
            result.balanceCents = user.value("balance_cents").toLongLong();
            result.idempotent = true;
            return finish();
        }
        q.finish();
        const qint64 balance = user.value("balance_cents").toLongLong();
        if (balance < 0 || balance > charging::model::kMaximumJsonSafeInteger - in.amountCents)
            return failure(UserApiError::Invalid);
        result.balanceCents = balance + in.amountCents;
        if (!run(q, "UPDATE users SET balance_cents=:balance, updated_at=:now WHERE id=:uid",
                 {{"balance", result.balanceCents}, {"now", now}, {"uid", in.userId}})
            || q.numRowsAffected() != 1) return {};
        if (!run(q, "INSERT INTO recharge_records (transaction_no,user_id,amount_cents,balance_after_cents,status,created_at) "
                    "VALUES (:txn,:uid,:amount,:balance,'SUCCESS',:now)",
                 {{"txn", in.transactionNo}, {"uid", in.userId}, {"amount", in.amountCents},
                  {"balance", result.balanceCents}, {"now", now}})) return {};
        if (!run(q, "SELECT * FROM recharge_records WHERE transaction_no=:txn",
                 {{"txn", in.transactionNo}}) || !q.next()) return {};
        result.rows.append(row(q));
        q.finish();
        return finish();
    }

    // Reuse the existing state-machine expiry updates within this transaction.
    if (in.action != UserApiAction::RechargeRecords) {
        QString diagnostic;
        if (!repository_detail::expireReservationsInTransaction(database_, in.nowUtc, &diagnostic))
            return {};
    }
    QString from;
    QString columns;
    QString where;
    QString sort;
    QVariantMap binds;
    switch (in.action) {
    case UserApiAction::Stations: {
        QString keyword = in.keyword;
        keyword.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_");
        from = "stations s";
        columns = "s.*, (SELECT COUNT(*) FROM chargers c WHERE c.station_id=s.id) AS total_chargers, "
                  "(SELECT COUNT(*) FROM chargers c WHERE c.station_id=s.id AND c.status='AVAILABLE') AS available_chargers";
        where = "s.status='ACTIVE' AND (s.name LIKE :keyword ESCAPE '\\' OR s.address LIKE :keyword ESCAPE '\\')";
        binds.insert("keyword", "%" + keyword + "%");
        sort = "s.id ASC";
        break;
    }
    case UserApiAction::Chargers:
        if (in.stationId <= 0) return failure(UserApiError::Invalid);
        if (!run(q, "SELECT id FROM stations WHERE id=:sid AND status='ACTIVE'", {{"sid", in.stationId}})) return {};
        if (!q.next()) return failure(UserApiError::NotFound);
        q.finish();
        from = "chargers c"; columns = "c.*"; where = "c.station_id=:sid";
        binds.insert("sid", in.stationId); sort = "c.id ASC";
        break;
    case UserApiAction::Reservations:
        from = "reservations r JOIN chargers c ON c.id=r.charger_id JOIN stations s ON s.id=c.station_id "
               "LEFT JOIN orders o ON o.reservation_id=r.id AND o.user_id=r.user_id";
        columns = "r.*, s.name AS station_name, c.code AS charger_code, o.id AS order_id";
        where = "r.user_id=:uid";
        sort = "r.reserved_at DESC,r.id DESC";
        if (!in.status.isEmpty()) { where += " AND r.status=:status"; binds.insert("status", in.status); }
        binds.insert("uid", in.userId);
        break;
    case UserApiAction::Orders:
        from = "orders o JOIN chargers c ON c.id=o.charger_id JOIN stations s ON s.id=c.station_id";
        columns = "o.*, s.name AS station_name, c.code AS charger_code";
        where = "o.user_id=:uid"; sort = "o.created_at DESC,o.id DESC";
        if (!in.status.isEmpty()) { where += " AND o.status=:status"; binds.insert("status", in.status); }
        binds.insert("uid", in.userId);
        break;
    case UserApiAction::RechargeRecords:
        from = "recharge_records r"; columns = "r.*"; where = "r.user_id=:uid";
        sort = "r.created_at DESC,r.id DESC"; binds.insert("uid", in.userId);
        break;
    default: return failure(UserApiError::Invalid);
    }
    if (!run(q, "SELECT COUNT(*) FROM " + from + " WHERE " + where, binds) || !q.next()) return {};
    const qint64 count = q.value(0).toLongLong();
    q.finish();
    if (count < 0 || count > std::numeric_limits<int>::max()) return failure(UserApiError::TooManyRows);
    result.total = static_cast<int>(count);
    binds.insert("limit", in.pageSize);
    binds.insert("offset", (qint64(in.page) - 1) * in.pageSize);
    if (!run(q, "SELECT " + columns + " FROM " + from + " WHERE " + where + " ORDER BY " + sort
                 + " LIMIT :limit OFFSET :offset", binds)) return {};
    while (q.next()) result.rows.append(row(q));
    if (q.lastError().isValid()) return {};
    q.finish();
    return finish();
}
} // namespace charging::server
