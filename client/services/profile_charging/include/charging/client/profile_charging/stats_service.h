#pragma once

#include "charging/client/profile_charging/i_request_transport.h"

#include <QObject>
#include <QVariantList>

namespace charging::client {

// Monthly usage summary for the 充电月报 page. Wire action: GET_USER_STATS
// (contract addition 2026-09-08). The rows carry no database model, so the
// contract shape travels verbatim as maps:
// [{monthKey, orderCount, energyWh, amountCents, durationSeconds, co2Grams}]
// newest month first. co2Grams is derived server-side from the grid average
// factor — the client never recomputes it.
class StatsService final : public QObject
{
    Q_OBJECT

public:
    explicit StatsService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isFetchingStats() const;
    void fetchStats(int months = 6);   // 1..12; server rejects out of range

signals:
    void statsLoaded(const QVariantList& months);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    IRequestTransport* transport_ = nullptr;
    bool fetching_ = false;
};

} // namespace charging::client
