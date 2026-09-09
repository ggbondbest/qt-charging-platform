#include "user_api_repository.h"
#include "charging_repository.h"
#include "charging_target_repository.h"
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
    for (int i = 0; i < record.count(); ++i)
        result.insert(record.fieldName(i), record.fieldName(i) == QStringLiteral("maintenance")
                          ? QVariant(query.value(i).toBool()) : query.value(i));
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
        // Recharge-reward coupon: rule constants live in user_api_contract.h,
        // TODO(contract): business sign-off pending. Idempotent replays returned
        // above, so one SUCCESS recharge grants at most one coupon.
        if (in.amountCents >= charging::protocol::user_api::kCouponRechargeThresholdCents) {
            const QString expires = in.nowUtc
                .addDays(charging::protocol::user_api::kCouponValidityDays)
                .toUTC().toString(Qt::ISODateWithMs);
            if (!run(q, "INSERT INTO coupons (user_id,kind,title,value_cents,threshold_cents,"
                        "status,source,expires_at,created_at,updated_at) "
                        "VALUES (:uid,'CASH','充值回馈 ¥5 充电券',:value,0,'AVAILABLE','充值回馈',"
                        ":expires,:now,:now)",
                     {{"uid", in.userId},
                      {"value", charging::protocol::user_api::kCouponValueCents},
                      {"expires", expires}, {"now", now}})) return {};
        }
        return finish();
    }
    if (in.action == UserApiAction::Stats) {
        if (in.months < 1 || in.months > charging::protocol::user_api::kMaximumStatsMonths)
            return failure(UserApiError::Invalid);
        // 聚合档（2026-09-08 批次B）：period 已过 normalize 白名单；空串按
        // "month" 兜底（同 service 缺省），三种格式串都是单点常量、不外泄。
        // 周档用 %W（年度周序号，周一为始，00-53）而非 ISO %G-%V——后者要
        // SQLite 3.44+，本机 3.37 直接返回 NULL（实测），跨年归属差异作为
        // 展示口径记录在契约文档。
        const QString period = in.period.isEmpty() ? QStringLiteral("month") : in.period;
        const char* format = "%Y-%m";
        if (period == QLatin1String("week")) format = "%Y-W%W";
        else if (period == QLatin1String("year")) format = "%Y";
        else if (period != QLatin1String("month")) return failure(UserApiError::Invalid);
        if (!run(q, QStringLiteral(
                    "SELECT strftime('%1', created_at) AS month_key, "
                    "COUNT(*) AS order_count, SUM(energy_wh) AS energy_wh, "
                    "SUM(amount_cents) AS amount_cents, SUM(duration_seconds) AS duration_seconds "
                    "FROM orders WHERE user_id=:uid AND status='COMPLETED' "
                    "GROUP BY month_key ORDER BY month_key DESC LIMIT :months").arg(format),
                 {{"uid", in.userId}, {"months", in.months}})) return {};
        while (q.next()) result.rows.append(row(q));
        if (q.lastError().isValid()) return {};
        q.finish();
        return finish();
    }
    if (in.action == UserApiAction::CheckIn) {
        // 批次C（2026-09-08）：日粒度幂等——user_checkins (user_id, day) 主键
        // + INSERT OR IGNORE；changed==0 即当日已签（返回现总分、gained=0，
        // 重放不算错误，RECHARGE 幂等同款语义）。day=UTC 日历日，与响应 day
        // 字段单点同源。总分为 SUM(points_ledger)——单一事实源，无余额列。
        const QString day = in.nowUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd"));
        if (!run(q, "INSERT OR IGNORE INTO user_checkins (user_id, day, created_at) "
                    "VALUES (:uid,:day,:now)",
                 {{"uid", in.userId}, {"day", day}, {"now", now}})) return {};
        if (q.numRowsAffected() == 0) {
            result.alreadyCheckedIn = true;
        } else if (!run(q, "INSERT INTO points_ledger (user_id, amount, reason, created_at) "
                           "VALUES (:uid,:amount,'CHECK_IN',:now)",
                        {{"uid", in.userId},
                         {"amount", charging::protocol::user_api::kCheckInRewardPoints},
                         {"now", now}})) return {};
        q.finish();
        if (!run(q, "SELECT COALESCE(SUM(amount), 0) AS points FROM points_ledger "
                    "WHERE user_id=:uid", {{"uid", in.userId}}) || !q.next()) return {};
        result.points = q.value(0).toLongLong();
        result.pointsGained = result.alreadyCheckedIn
            ? 0 : charging::protocol::user_api::kCheckInRewardPoints;
        q.finish();
        return finish();
    }
    if (in.action == UserApiAction::GetPoints) {
        // 批次C：流水分页（新→旧）+ 总分单查合一（同 COUNT 顺路 SUM，一条 SQL）。
        if (!run(q, "SELECT COUNT(*) AS cnt, COALESCE(SUM(amount), 0) AS points "
                    "FROM points_ledger WHERE user_id=:uid", {{"uid", in.userId}})
            || !q.next()) return {};
        const qint64 count = q.value("cnt").toLongLong();
        result.points = q.value("points").toLongLong();
        q.finish();
        if (count < 0 || count > std::numeric_limits<int>::max())
            return failure(UserApiError::TooManyRows);
        result.total = static_cast<int>(count);
        if (!run(q, "SELECT * FROM points_ledger WHERE user_id=:uid "
                    "ORDER BY created_at DESC, id DESC LIMIT :limit OFFSET :offset",
                 {{"uid", in.userId}, {"limit", in.pageSize},
                  {"offset", (qint64(in.page) - 1) * in.pageSize}})) return {};
        while (q.next()) result.rows.append(row(q));
        if (q.lastError().isValid()) return {};
        q.finish();
        return finish();
    }
    if (in.action == UserApiAction::SubmitRating) {
        // 批次E（2026-09-08）：一单一评，只对本人 COMPLETED 订单开放（防越权/
        // 防给未完成订单刷评）。charger_id 取订单快照，不信任客户端传参。
        if (in.orderId <= 0 || in.rating < charging::protocol::user_api::kMinimumRating ||
            in.rating > charging::protocol::user_api::kMaximumRating)
            return failure(UserApiError::Invalid);
        if (!run(q, "SELECT charger_id FROM orders WHERE id=:oid AND user_id=:uid "
                    "AND status='COMPLETED'", {{"oid", in.orderId}, {"uid", in.userId}})
            || !q.next())
            return failure(UserApiError::NotFound);
        const qint64 chargerId = q.value(0).toLongLong();
        q.finish();
        // 幂等锚 order_id UNIQUE + INSERT OR IGNORE：同单重投不报错也不覆盖首评
        // （CHECK_IN/RECHARGE 同款重放语义）。改评能力 TODO(contract) 二期。
        if (!run(q, "INSERT OR IGNORE INTO charger_ratings "
                    "(user_id, charger_id, order_id, rating, comment, created_at) "
                    "VALUES (:uid,:cid,:oid,:rating,:comment,:now)",
                 {{"uid", in.userId}, {"cid", chargerId}, {"oid", in.orderId},
                  {"rating", in.rating}, {"comment", in.comment}, {"now", now}})) return {};
        result.alreadyRated = (q.numRowsAffected() == 0);
        q.finish();
        // 回读当前落库行（新插入或既有首评），响应形与 GET_MY_RATINGS 同构。
        if (!run(q, "SELECT r.*, c.code AS charger_code, s.name AS station_name "
                    "FROM charger_ratings r JOIN chargers c ON c.id=r.charger_id "
                    "JOIN stations s ON s.id=c.station_id WHERE r.order_id=:oid",
                 {{"oid", in.orderId}}) || !q.next()) return {};
        result.rows.append(row(q));
        q.finish();
        return finish();
    }
    if (in.action == UserApiAction::GetMyRatings) {
        // 批次E：本人评价流水（新→旧）分页；联查桩号/站名让"我的评价"页自成一体。
        if (!run(q, "SELECT COUNT(*) FROM charger_ratings WHERE user_id=:uid",
                 {{"uid", in.userId}}) || !q.next()) return {};
        const qint64 count = q.value(0).toLongLong();
        q.finish();
        if (count < 0 || count > std::numeric_limits<int>::max())
            return failure(UserApiError::TooManyRows);
        result.total = static_cast<int>(count);
        if (!run(q, "SELECT r.*, c.code AS charger_code, s.name AS station_name "
                    "FROM charger_ratings r JOIN chargers c ON c.id=r.charger_id "
                    "JOIN stations s ON s.id=c.station_id WHERE r.user_id=:uid "
                    "ORDER BY r.created_at DESC, r.id DESC LIMIT :limit OFFSET :offset",
                 {{"uid", in.userId}, {"limit", in.pageSize},
                  {"offset", (qint64(in.page) - 1) * in.pageSize}})) return {};
        while (q.next()) result.rows.append(row(q));
        if (q.lastError().isValid()) return {};
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
        from = "chargers c";
        columns = "c.*,EXISTS(SELECT 1 FROM repair_reports rp WHERE rp.charger_id=c.id "
                  "AND rp.status IN ('ACCEPTED','PROCESSING')) AS maintenance";
        where = "c.station_id=:sid";
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
    case UserApiAction::Coupons: {
        // 审查 P2#4：EXPIRED 是派生态——存储列只在领/用时改写，到期没有写
        // 作业。过滤、total、响应 status 统一按有效态口径：USED 最优先，
        // 其次到期 EXPIRED，其余 AVAILABLE；页面与客户端不再二次派生。
        // 时间列为 UTC ISO-8601 定长文本（:now 同款格式），字典序即时间序。
        const QString effectiveStatus =
            "(CASE WHEN c.status = 'USED' THEN 'USED' "
            "WHEN c.expires_at <= :now THEN 'EXPIRED' ELSE 'AVAILABLE' END)";
        from = "coupons c";
        // 显式列清单：存储 status 不出网，派生值以同名 status 输出（契约字段不变）。
        columns = "c.id,c.user_id,c.kind,c.title,c.value_cents,c.discount_tenths,"
                  "c.threshold_cents,c.source,c.expires_at,c.created_at,c.updated_at,"
                  + effectiveStatus + " AS status";
        where = "c.user_id=:uid";
        sort = "c.created_at DESC,c.id DESC";
        binds.insert("uid", in.userId);
        binds.insert("now", now);
        if (!in.status.isEmpty()) {
            where += " AND " + effectiveStatus + " = :status";
            binds.insert("status", in.status.toUpper());   // wire lowercase → stored uppercase
        }
        break;
    }
    case UserApiAction::Notifications:
        from = "notifications n"; columns = "n.*"; where = "n.user_id=:uid";
        sort = "n.created_at DESC,n.id DESC"; binds.insert("uid", in.userId);
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
    while (q.next()) {
        auto value = row(q);
        if (in.action == UserApiAction::Orders) {
            QJsonObject target;
            QString stopReason;
            if (!chargingTargetDto(database_, value.value(QStringLiteral("id")).toLongLong(),
                                   &target, &stopReason)) return {};
            value.insert(QStringLiteral("target"), target.toVariantMap());
            if (!stopReason.isEmpty()) value.insert(QStringLiteral("stop_reason"), stopReason);
            else value.remove(QStringLiteral("stop_reason"));
        }
        result.rows.append(value);
    }
    if (q.lastError().isValid()) return {};
    q.finish();
    return finish();
}
} // namespace charging::server
