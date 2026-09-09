#include "admin_order_billing.h"

#include "charging/common/model/models.h"

#include <QJsonArray>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace charging::server {

bool calculateEnergyFeeCents(qint64 energyWh, qint64 unitPriceCentsPerKwh, qint64* amountCents)
{
    const qint64 maximum = charging::model::kMaximumJsonSafeInteger;
    if (!amountCents || energyWh < 0 || energyWh > maximum ||
        unitPriceCentsPerKwh < 0 || unitPriceCentsPerKwh > maximum)
        return false;
    const qint64 wholeKwh = energyWh / 1000;
    if (unitPriceCentsPerKwh && wholeKwh > maximum / unitPriceCentsPerKwh)
        return false;
    const qint64 wholeAmount = wholeKwh * unitPriceCentsPerKwh;
    // remainder <= 999: its product with a JSON-safe integer fits qint64.
    const qint64 fractionalAmount = ((energyWh % 1000) * unitPriceCentsPerKwh + 500) / 1000;
    if (fractionalAmount > maximum - wholeAmount)
        return false;
    *amountCents = wholeAmount + fractionalAmount;
    return true;
}

bool orderBillingDto(const QSqlDatabase& database, qint64 orderId, QJsonObject* fields,
                     QString* diagnostic)
{
    if (diagnostic) diagnostic->clear();
    if (!fields) return false;
    *fields = {{QStringLiteral("billingAvailability"), QStringLiteral("UNAVAILABLE")},
               {QStringLiteral("feeBreakdown"), QJsonValue::Null},
               {QStringLiteral("pricingSnapshot"), QJsonValue::Null}};
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT o.status, o.started_at, o.stopped_at, o.energy_wh, o.amount_cents, "
        "o.telemetry_captured_at, p.version, p.unit_price_cents_per_kwh, p.captured_at "
        "FROM orders o LEFT JOIN order_pricing_snapshots p ON p.order_id = o.id WHERE o.id = ?"));
    query.addBindValue(orderId);
    if (!query.exec() || !query.next()) {
        if (diagnostic) *diagnostic = QStringLiteral("Unable to load order billing snapshot");
        return false;
    }
    const QString status = query.value(0).toString();
    const bool estimated = status == QStringLiteral("CHARGING");
    fields->insert(QStringLiteral("estimated"), estimated);
    if (query.value(6).isNull()) return true;
    const QString version = query.value(6).toString();
    const qint64 energyWh = query.value(3).toLongLong();
    const qint64 unitPrice = query.value(7).toLongLong();
    qint64 energyFee = 0;
    if (version != QStringLiteral("energy-only-v1") ||
        !calculateEnergyFeeCents(energyWh, unitPrice, &energyFee) ||
        energyFee != query.value(4).toLongLong()) {
        if (diagnostic) *diagnostic = QStringLiteral("Stored billing snapshot is inconsistent");
        return false;
    }
    QJsonArray segments;
    if (!query.value(1).isNull()) {
        const QVariant end = estimated ? query.value(5) : query.value(2);
        segments.append(QJsonObject{
            {QStringLiteral("startAt"), query.value(1).toString()},
            {QStringLiteral("endAt"), end.isNull() ? QJsonValue(QJsonValue::Null)
                                                   : QJsonValue(end.toString())},
            {QStringLiteral("energyWh"), energyWh},
            {QStringLiteral("unitPriceCentsPerKwh"), unitPrice},
            {QStringLiteral("amountCents"), energyFee}});
    }
    fields->insert(QStringLiteral("billingAvailability"), QStringLiteral("AVAILABLE"));
    fields->insert(QStringLiteral("pricingSnapshot"), QJsonObject{
        {QStringLiteral("version"), version},
        {QStringLiteral("capturedAt"), query.value(8).toString()},
        {QStringLiteral("segments"), segments}});
    fields->insert(QStringLiteral("feeBreakdown"), QJsonObject{
        {QStringLiteral("energyFeeCents"), energyFee},
        {QStringLiteral("serviceFeeCents"), 0},
        {QStringLiteral("parkingFeeCents"), 0},
        {QStringLiteral("discountCents"), 0},
        {QStringLiteral("payableCents"), energyFee},
        {QStringLiteral("paidCents"), status == QStringLiteral("COMPLETED") ? energyFee : 0},
        {QStringLiteral("currency"), QStringLiteral("CNY")}});
    return true;
}

} // namespace charging::server
