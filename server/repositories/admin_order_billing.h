#pragma once

#include <QJsonObject>
#include <QSqlDatabase>

namespace charging::server {

// Integer Wh -> cents, round-half-up. Returns false on invalid/unsafe values.
bool calculateEnergyFeeCents(qint64 energyWh, qint64 unitPriceCentsPerKwh, qint64* amountCents);

// Read only; caller owns the database thread/transaction. Never queries a
// station's current tariff or invents snapshots for orders predating v4.
bool orderBillingDto(const QSqlDatabase& database, qint64 orderId, QJsonObject* fields,
                     QString* diagnostic = nullptr);

} // namespace charging::server
