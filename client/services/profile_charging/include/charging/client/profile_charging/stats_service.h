#pragma once

#include "charging/client/profile_charging/i_request_transport.h"

#include <QObject>
#include <QVariantList>

namespace charging::client {

// Monthly usage summary for the 充电月报 page. Wire action: GET_USER_STATS
// (contract addition 2026-09-08; 批次B adds the period granularity). The
// rows carry no database model, so the contract shape travels verbatim as
// maps: [{monthKey, orderCount, energyWh, amountCents, durationSeconds,
// co2Grams}] newest period first. monthKey is "YYYY-MM" by default,
// "YYYY-Www" for period=week, "YYYY" for period=year. co2Grams is derived
// server-side from the grid average factor — the client never recomputes it.
class StatsService final : public QObject
{
    Q_OBJECT

public:
    explicit StatsService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isFetchingStats() const;
    void fetchStats(int months = 6, const QString& period = QStringLiteral("month"));
    // months = 期数窗口 1..12；period ∈ "week"|"month"|"year"，其余值由
    // 服务端 normalize 拒绝（客户端不重复校验——同一份白名单只住一处）。

signals:
    void statsLoaded(const QVariantList& months);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    IRequestTransport* transport_ = nullptr;
    bool fetching_ = false;
};

} // namespace charging::client
