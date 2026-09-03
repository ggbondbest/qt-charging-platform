#include "charging/client/profile_charging/presentation_format.h"

#include <QRegularExpression>

namespace charging::client {

QString formatCentsAsYuan(qint64 cents)
{
    const bool negative = cents < 0;
    const qint64 absolute = negative ? -cents : cents;
    const QString text = QStringLiteral("%1.%2")
                             .arg(absolute / 100)
                             .arg(absolute % 100, 2, 10, QChar('0'));
    return negative ? QStringLiteral("-%1").arg(text) : text;
}

QString formatDateTimeLocal(const QDateTime& utcValue)
{
    if (!utcValue.isValid()) {
        return QStringLiteral("--");
    }
    return utcValue.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString formatDurationHms(qint64 totalSeconds)
{
    const qint64 safeSeconds = totalSeconds > 0 ? totalSeconds : 0;
    const qint64 hours = safeSeconds / 3600;
    const qint64 minutes = (safeSeconds % 3600) / 60;
    const qint64 seconds = safeSeconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString formatEnergyWhAsKwh(qint64 energyWh)
{
    const qint64 safeWh = energyWh > 0 ? energyWh : 0;
    const qint64 hundredths = (safeWh * 100 + 500) / 1000; // half-up to 0.01 kWh
    return QStringLiteral("%1.%2")
        .arg(hundredths / 100)
        .arg(hundredths % 100, 2, 10, QChar('0'));
}

QString formatCentsPerKwh(qint64 priceCentsPerKwh)
{
    const qint64 safePrice = priceCentsPerKwh > 0 ? priceCentsPerKwh : 0;
    return QStringLiteral("%1.%2")
        .arg(safePrice / 100)
        .arg(safePrice % 100, 2, 10, QChar('0'));
}

QString formatWattsAsKw(qint64 powerWatts)
{
    const qint64 safeWatts = powerWatts > 0 ? powerWatts : 0;
    const qint64 tenths = (safeWatts * 10 + 500) / 1000; // half-up to 0.1 kW
    return QStringLiteral("%1.%2").arg(tenths / 10).arg(tenths % 10);
}

bool parseYuanTextToCents(const QString& text, qint64* outCents)
{
    if (outCents == nullptr) {
        return false;
    }

    static const QRegularExpression pattern(
        QStringLiteral("^(0|[1-9][0-9]{0,7})(?:\\.([0-9]{1,2}))?$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch()) {
        return false;
    }

    const qint64 yuan = match.captured(1).toLongLong();
    const QString fraction = match.captured(2);
    qint64 remainderCents = 0;
    if (!fraction.isEmpty()) {
        remainderCents = fraction.toLongLong();
        if (fraction.size() == 1) {
            remainderCents *= 10;
        }
    }

    // Keep the result inside the JSON-safe integer band used across the wire.
    if (yuan > 9007199254740991LL / 100) {
        return false;
    }

    const qint64 cents = yuan * 100 + remainderCents;
    if (cents <= 0) {
        return false;
    }
    *outCents = cents;
    return true;
}

} // namespace charging::client
