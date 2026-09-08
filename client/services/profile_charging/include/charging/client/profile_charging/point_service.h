#pragma once

#include "charging/client/profile_charging/i_request_transport.h"

#include <QObject>
#include <QVariantList>

namespace charging::client {

// 每日签到 + 积分流水（2026-09-08 批次C）。Wire actions: CHECK_IN（写型，
// 日粒度幂等，服务端 user_checkins 主键即锁）、GET_POINTS（分页读流水）。
// entries 行 = GET_POINTS 响应形：[{id, amount, reason, createdAtUtc}] 新→旧；
// points 为服务端 SUM 单一事实源，客户端永不累加对账。
class PointService final : public QObject
{
    Q_OBJECT

public:
    explicit PointService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isBusy() const;              // 两请求共用的单飞旗标（同 StatsService 口径）
    void fetchPoints(int page = 1, int pageSize = 20);
    void checkIn();

signals:
    void pointsLoaded(qint64 points, const QVariantList& entries, int total);
    void checkInCompleted(const QString& day, qint64 points, qint64 gained,
                          bool alreadyCheckedIn);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    IRequestTransport* transport_ = nullptr;
    bool busy_ = false;
};

} // namespace charging::client
