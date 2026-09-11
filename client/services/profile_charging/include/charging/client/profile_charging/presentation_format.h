#pragma once

#include <QDateTime>
#include <QString>
#include <QtGlobal>

namespace charging::client {

// Display helpers for the wire units fixed by the architecture contract:
// money is integer cents, energy is Wh, duration is seconds.

// 123456 -> "1234.56"; never introduces floating point.
QString formatCentsAsYuan(qint64 cents);

// "2026-09-01 16:30" in local time (UTC on the wire).
QString formatDateTimeLocal(const QDateTime& utcValue);

// 5538 -> "01:32:18".
QString formatDurationHms(qint64 totalSeconds);

// 24680 Wh -> "24.68" (kWh, integer-rounded to two decimals).
QString formatEnergyWhAsKwh(qint64 energyWh);

// 132 cents/kWh -> "1.32".
QString formatCentsPerKwh(qint64 priceCentsPerKwh);

// 6500 W -> "6.5" (kW, integer-rounded to one decimal).
QString formatWattsAsKw(qint64 powerWatts);

// Parses user-typed yuan text like "50" or "50.25" into cents.
// Rejects three-decimal fractions and zero amounts.
bool parseYuanTextToCents(const QString& text, qint64* outCents);

} // namespace charging::client
