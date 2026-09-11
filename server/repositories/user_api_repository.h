#pragma once

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QVariantMap>

namespace charging::server {

// Application-specific queries, separate from the member-5 admin repositories.
// Rows use SQL column names; only the Service knows the wire representation.
// 批次C（2026-09-08）：CheckIn 写型日幂等（user_checkins 主键即幂等锁），
// GetPoints 读积分流水 + SUM 总分。
// 批次E（2026-09-08）：SubmitRating 写评价（order_id UNIQUE 即幂等锁，一单一评），
// GetMyRatings 分页读本人评价流水（联查桩号/站名）。
// 2026-09-09 需求批：CreditLevelReward 升级礼包入账（金额服务端单点推导，
// (user_id,'LEVEL_GIFT',amount) 流水行去重幂等——各档金额互不相同天然可辨）。
enum class UserApiAction { Stations, Chargers, Reservations, Profile, UpdateProfile,
                           Recharge, RechargeRecords, Orders, Stats, Coupons, Notifications,
                           CheckIn, GetPoints, SubmitRating, GetMyRatings,
                           CreditLevelReward };
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
    int months = 6;               // Stats window (1..kMaximumStatsMonths)
    QString period;               // Stats 聚合档 "week"|"month"|"year"（2026-09-08 批次B；空=month）
    // 批次E：SUBMIT_CHARGER_RATING 入参（订单须本人 COMPLETED，业务在服务端把关）。
    qint64 orderId = 0;
    int rating = 0;               // 1..5
    QString comment;              // 可空串（已 trim、≤140，normalize 保证）
    // 2026-09-09 需求批：CREDIT_LEVEL_REWARD 入参（2..5，normalize 已把关；
    // 礼包金额由此档在 levelRewardPoints() 单点推导，入参不含金额）。
    int level = 0;
    QDateTime nowUtc;
};
struct UserApiResult {
    UserApiError error = UserApiError::Database;
    QList<QVariantMap> rows;
    int total = 0;
    qint64 balanceCents = 0;
    bool idempotent = false;
    // 批次C（2026-09-08）签到/积分：points=当前总分（SUM 单一事实源）；
    // CheckIn 专用 gained（重放=0）与 alreadyCheckedIn。GetPoints 复用
    // points + rows 流水 + total 分页。
    qint64 points = 0;
    qint64 pointsGained = 0;
    bool alreadyCheckedIn = false;
    // 批次E（2026-09-08）评价：SubmitRating 幂等重放标记（rows[0]=落库评价行）。
    bool alreadyRated = false;
    // 2026-09-09 需求批：CreditLevelReward 同档重放标记（重发 gained=0，不算错误，
    // CHECK_IN 同款语义）；points=当前总分（SUM 单一事实源）。
    bool alreadyCredited = false;
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
