#pragma once

#include "charging/client/profile_charging/i_request_transport.h"

#include <QObject>
#include <QVariantList>

namespace charging::client {

// 每日签到 + 积分流水（2026-09-08 批次C）。Wire actions: CHECK_IN（写型，
// 日粒度幂等，服务端 user_checkins 主键即锁）、GET_POINTS（分页读流水）、
// CREDIT_LEVEL_REWARD（2026-09-09 需求批：升级礼包入账，只带 level，金额服务端
// 单点推导；刻意不走 busy_ 单飞闸——升级时刻常伴随流水刷新，礼包被静默丢弃
// 即是"积分没到账"bug 本身；幂等在服务端流水去重，与并发刷新互不冲突）。
// entries 行 = GET_POINTS 响应形：[{id, amount, reason, createdAtUtc}] 新→旧；
// points 为服务端 SUM 单一事实源，客户端永不累加对账。
class PointService final : public QObject
{
    Q_OBJECT

public:
    explicit PointService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isBusy() const;              // fetch/checkIn 共用的单飞旗标（同 StatsService 口径）
    void fetchPoints(int page = 1, int pageSize = 20);
    void checkIn();
    void creditLevelReward(int level);   // level 2..5；越界由 normalize 拒绝走 operationFailed

signals:
    void pointsLoaded(qint64 points, const QVariantList& entries, int total);
    void checkInCompleted(const QString& day, qint64 points, qint64 gained,
                          bool alreadyCheckedIn);
    void levelRewardCredited(int level, qint64 points, qint64 gained, bool alreadyCredited);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    IRequestTransport* transport_ = nullptr;
    bool busy_ = false;
};

} // namespace charging::client
