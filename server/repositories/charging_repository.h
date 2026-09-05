#pragma once

#include "workflow_repository_types.h"

#include <QDateTime>
#include <QSqlDatabase>

namespace charging::server {

namespace repository_detail {
// Caller owns an active transaction; shared by workflow and user queries.
bool expireReservationsInTransaction(const QSqlDatabase& database, const QDateTime& nowUtc,
                                     QString* diagnostic);
}

class ChargingRepository final
{
public:
    explicit ChargingRepository(const QSqlDatabase& database);

    ChargingRepositoryResult reserve(qint64 userId, qint64 chargerId,
                                     const QDateTime& reservedAtUtc, const QDateTime& expiresAtUtc,
                                     const QString& orderNo) const;
    ChargingRepositoryResult cancelReservation(qint64 userId, qint64 reservationId,
                                               const QDateTime& cancelledAtUtc) const;
    ChargingRepositoryResult startCharging(qint64 userId, qint64 reservationId,
                                           const QDateTime& startedAtUtc) const;

    // Applies reservation expiry before reading. A CHARGING order reports its
    // persisted start time and charger power so the Service can calculate a
    // real-time, non-persisted meter snapshot.
    ChargingRepositoryResult chargingStatus(qint64 userId, qint64 orderId,
                                            const QDateTime& observedAtUtc) const;

    // expectedStartedAtUtc plus old-state WHERE clauses make the update safe
    // if two STOP requests race. A repeated STOP returns the stored result.
    ChargingRepositoryResult stopCharging(qint64 userId, qint64 orderId,
                                          const QDateTime& expectedStartedAtUtc,
                                          const QDateTime& stoppedAtUtc, qint64 durationSeconds,
                                          qint64 energyWh, qint64 amountCents) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
