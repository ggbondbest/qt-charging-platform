#include "billing_service.h"

#include "charging/common/model/models.h"

#include <limits>

namespace charging::server {

namespace {

charging::protocol::ProtocolError billingError(const QString& message)
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kInternalError);
    error.message = message;
    return error;
}

bool multiplyWouldOverflow(qint64 left, qint64 right)
{
    return left != 0 && right > std::numeric_limits<qint64>::max() / left;
}

} // namespace

BillingResult BillingService::calculate(int powerWatts, qint64 durationSeconds,
                                        qint64 unitPriceCentsPerKwh) const
{
    BillingResult result;
    result.durationSeconds = durationSeconds;

    const qint64 maximum = charging::model::kMaximumJsonSafeInteger;
    if (powerWatts <= 0 || durationSeconds < 0 || durationSeconds > maximum ||
        unitPriceCentsPerKwh < 0 || unitPriceCentsPerKwh > maximum) {
        result.error = billingError(QStringLiteral("计费参数无效"));
        return result;
    }

    const qint64 power = static_cast<qint64>(powerWatts);
    if (multiplyWouldOverflow(power, durationSeconds)) {
        result.error = billingError(QStringLiteral("充电计量结果超出系统范围"));
        return result;
    }

    const qint64 wattSeconds = power * durationSeconds;
    result.energyWh = wattSeconds / 3600;
    if (result.energyWh > maximum) {
        result.error = billingError(QStringLiteral("充电电量超出系统范围"));
        return result;
    }

    if (multiplyWouldOverflow(result.energyWh, unitPriceCentsPerKwh)) {
        result.error = billingError(QStringLiteral("充电金额超出系统范围"));
        return result;
    }
    const qint64 amountNumerator = result.energyWh * unitPriceCentsPerKwh;
    if (amountNumerator > std::numeric_limits<qint64>::max() - 500) {
        result.error = billingError(QStringLiteral("充电金额超出系统范围"));
        return result;
    }
    result.amountCents = (amountNumerator + 500) / 1000;
    if (result.amountCents > maximum) {
        result.error = billingError(QStringLiteral("充电金额超出系统范围"));
        return result;
    }

    result.success = true;
    return result;
}

} // namespace charging::server
