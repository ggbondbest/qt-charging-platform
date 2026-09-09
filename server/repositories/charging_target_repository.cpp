#include "charging_target_repository.h"

#include "admin_order_billing.h"
#include "charging/common/model/models.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <cmath>
#include <limits>

namespace charging::server {
namespace {
constexpr qint64 maximum = charging::model::kMaximumJsonSafeInteger;
bool fail(QString* diagnostic, const QString& message)
{
    if (diagnostic) *diagnostic = message;
    return false;
}

// floor(a*b/divisor), saturating only when the JSON-safe range is exceeded.
qint64 mulDiv(qint64 a, qint64 b, qint64 divisor)
{
    const qint64 whole = a / divisor;
    const qint64 rest = a % divisor;
    if (whole > maximum / b) return maximum;
    const qint64 remainder = (rest * b) / divisor;
    const qint64 result = whole * b;
    return result > maximum - remainder ? maximum : result + remainder;
}

qint64 firstBudgetWh(qint64 budget, qint64 price)
{
    qint64 low = 0, high = maximum;
    while (low < high) {
        const qint64 middle = low + (high - low) / 2;
        qint64 amount = 0;
        if (!calculateEnergyFeeCents(middle, price, &amount) || amount >= budget)
            high = middle;
        else
            low = middle + 1;
    }
    qint64 amount = 0;
    if (!calculateEnergyFeeCents(low, price, &amount) || amount > budget) --low;
    return qMax<qint64>(0, low);
}
}

bool validateChargingTarget(const QJsonObject& target, QString* diagnostic)
{
    if (diagnostic) diagnostic->clear();
    if (target.isEmpty()) return true;
    if (target.size() != 2 || !target.value(QStringLiteral("type")).isString()
        || !target.value(QStringLiteral("value")).isDouble())
        return fail(diagnostic, QStringLiteral("A charging target requires type and integer value"));
    const QString type = target.value(QStringLiteral("type")).toString();
    const double value = target.value(QStringLiteral("value")).toDouble();
    if ((type != QStringLiteral("AMOUNT") && type != QStringLiteral("ENERGY")
         && type != QStringLiteral("DURATION")) || !std::isfinite(value) || value < 1
        || value > maximum || std::floor(value) != value)
        return fail(diagnostic, QStringLiteral("Invalid charging target"));
    return true;
}

bool insertChargingTargetInTransaction(const QSqlDatabase& database, qint64 orderId,
                                      const QJsonObject& target, const QDateTime& createdAt,
                                      QString* diagnostic)
{
    if (!validateChargingTarget(target, diagnostic)) return false;
    if (target.isEmpty()) return true;
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO order_charge_targets "
                                  "(order_id, target_type, target_value, created_at) VALUES (?, ?, ?, ?)"));
    insert.addBindValue(orderId);
    insert.addBindValue(target.value(QStringLiteral("type")).toString());
    insert.addBindValue(static_cast<qint64>(target.value(QStringLiteral("value")).toDouble()));
    insert.addBindValue(createdAt.toUTC().toString(Qt::ISODateWithMs));
    return insert.exec() || fail(diagnostic, insert.lastError().text());
}

bool chargingTargetDto(const QSqlDatabase& database, qint64 orderId, QJsonObject* target,
                       QString* stopReason, QString* diagnostic)
{
    if (target) *target = {};
    if (stopReason) stopReason->clear();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT t.target_type, t.target_value, o.duration_seconds, "
                                 "o.energy_wh, o.amount_cents, o.stop_reason FROM orders o "
                                 "LEFT JOIN order_charge_targets t ON t.order_id = o.id WHERE o.id = ?"));
    query.addBindValue(orderId);
    if (!query.exec() || !query.next()) return fail(diagnostic, query.lastError().text());
    const QString reason = query.value(5).toString();
    if (stopReason) *stopReason = reason;
    if (query.value(0).isNull()) return true;
    const QString type = query.value(0).toString();
    const qint64 value = query.value(1).toLongLong();
    const qint64 completed = query.value(type == QStringLiteral("AMOUNT") ? 4
                                        : type == QStringLiteral("ENERGY") ? 3 : 2).toLongLong();
    if (target) {
        *target = {{QStringLiteral("type"), type}, {QStringLiteral("value"), value},
                   {QStringLiteral("completedValue"), completed},
                   {QStringLiteral("remainingValue"), qMax<qint64>(0, value - completed)},
                   {QStringLiteral("progressPercent"), value > 0 ? qMin(100.0, completed * 100.0 / value) : 0.0},
                   {QStringLiteral("reached"), reason.startsWith(QStringLiteral("TARGET_"))}};
    }
    return true;
}

bool activeChargingTargetOrders(const QSqlDatabase& database,
                                QVector<QPair<qint64, qint64>>* orders, QString* diagnostic)
{
    orders->clear();
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT o.user_id, o.id FROM orders o JOIN order_charge_targets t "
                                   "ON t.order_id = o.id WHERE o.status = 'CHARGING' ORDER BY o.id")))
        return fail(diagnostic, query.lastError().text());
    while (query.next()) orders->append({query.value(0).toLongLong(), query.value(1).toLongLong()});
    return true;
}

TargetMeterSample calculateTargetSample(int powerWatts, qint64 elapsedSeconds,
                                       qint64 price, const QJsonObject& target)
{
    TargetMeterSample sample;
    // DTO contains extra progress fields; extract only the frozen definition.
    const QJsonObject definition{{QStringLiteral("type"), target.value(QStringLiteral("type"))},
                                 {QStringLiteral("value"), target.value(QStringLiteral("value"))}};
    if (!validateChargingTarget(definition) || elapsedSeconds < 0 || elapsedSeconds > maximum
        || powerWatts <= 0 || price < 0 || price > maximum) return sample;
    const QString type = definition.value(QStringLiteral("type")).toString();
    const qint64 value = static_cast<qint64>(definition.value(QStringLiteral("value")).toDouble());
    sample.durationSeconds = elapsedSeconds;
    if (type == QStringLiteral("DURATION")) {
        sample.reached = elapsedSeconds >= value;
        sample.durationSeconds = qMin(elapsedSeconds, value);
        sample.energyWh = mulDiv(sample.durationSeconds, powerWatts, 3600);
    } else {
        if (type == QStringLiteral("AMOUNT") && price == 0) return sample;
        const qint64 limitWh = type == QStringLiteral("ENERGY") ? value : firstBudgetWh(value, price);
        const qint64 naturalWh = mulDiv(elapsedSeconds, powerWatts, 3600);
        sample.reached = naturalWh >= limitWh;
        sample.energyWh = qMin(naturalWh, limitWh);
        if (sample.reached) {
            // Integer seconds are a presentation meter. The last partial
            // second is rounded up, while Wh and fee remain exact and capped.
            const qint64 floorSeconds = mulDiv(limitWh, 3600, powerWatts);
            const qint64 remainder = (limitWh % powerWatts) * 3600 % powerWatts;
            sample.durationSeconds = qMin(elapsedSeconds, floorSeconds + (remainder > 0 ? 1 : 0));
        }
    }
    if (!calculateEnergyFeeCents(sample.energyWh, price, &sample.amountCents)) return sample;
    if (type == QStringLiteral("AMOUNT") && sample.amountCents > value) return sample;
    if (sample.reached) sample.stopReason = QStringLiteral("TARGET_") + type;
    sample.success = true;
    return sample;
}

} // namespace charging::server
