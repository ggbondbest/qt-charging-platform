#include "admin_charger_extensions.h"

#include "admin_repository.h"
#include "admin_order_billing.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariantList>
#include <cmath>

namespace charging::server {
namespace {
constexpr qint64 maximumInteger = 9007199254740991LL;
void require(bool valid)
{
    if (!valid)
        throw AdminFailure("INVALID_ARGUMENT");
}
void fields(const QJsonObject& p, const QStringList& allowed)
{
    for (auto it = p.begin(); it != p.end(); ++it)
        require(allowed.contains(it.key()));
}
void id(const QJsonObject& p, const QString& key)
{
    bool valid = false;
    const auto number = p.value(key).toString().toLongLong(&valid);
    require(p.value(key).isString() && valid && number > 0 &&
            QString::number(number) == p.value(key).toString());
}
void timestamp(const QJsonObject& p, const QString& key)
{
    const auto text = p.value(key).toString();
    const auto parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    require(p.value(key).isString() && text.size() == 24 && parsed.isValid() &&
            parsed.toUTC().toString(Qt::ISODateWithMs) == text);
}
void integer(const QJsonObject& p, const QString& key, int maximum)
{
    const auto value = p.value(key);
    require(value.isDouble() && std::isfinite(value.toDouble()) &&
            std::floor(value.toDouble()) == value.toDouble() && value.toDouble() >= 1 &&
            value.toDouble() <= maximum);
}
QSqlQuery execute(const QSqlDatabase& db, const QString& sql, const QVariantList& values = {})
{
    QSqlQuery query(db);
    if (!query.prepare(sql))
        throw AdminFailure("DATABASE_ERROR");
    for (const auto& value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        const int code = query.lastError().nativeErrorCode().toInt() & 0xff;
        throw AdminFailure(code == 19 || code == 5 || code == 6 ? "CONFLICT" : "DATABASE_ERROR");
    }
    return query;
}
qint64 count(const QSqlDatabase& db, const QString& sql, const QVariantList& values = {})
{
    auto query = execute(db, sql, values);
    if (!query.next())
        throw AdminFailure("DATABASE_ERROR");
    return query.value(0).toLongLong();
}
QJsonValue nullable(const QVariant& value)
{
    return value.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(value.toString());
}
QString exceptionColumns()
{
    return QStringLiteral("id,charger_id,code,severity,safe_summary,status,occurred_at,"
                          "acknowledged_at,recovered_at,recoverable,recovery_action,"
                          "recovered_by_admin_id,recovery_command_id,recovery_message,updated_at");
}
QJsonObject exceptionDto(const QSqlQuery& query)
{
    // Only these fields leave the repository. No diagnostic payload or raw log
    // exists in this DTO, and summaries originate from the server's enum map.
    return {{"id", query.value(0).toString()},
            {"chargerId", query.value(1).toString()},
            {"code", query.value(2).toString()},
            {"severity", query.value(3).toString()},
            {"safeSummary", query.value(4).toString()},
            {"status", query.value(5).toString()},
            {"occurredAt", query.value(6).toString()},
            {"acknowledgedAt", nullable(query.value(7))},
            {"recoveredAt", nullable(query.value(8))},
            {"recoverable", query.value(9).toInt() == 1},
            {"recoveryAction", query.value(10).toString()},
            {"recoveredByAdminId", nullable(query.value(11))},
            {"recoveryCommandId", nullable(query.value(12))},
            {"recoveryMessage", nullable(query.value(13))},
            {"updatedAt", query.value(14).toString()},
            {"simulated", true}};
}
QJsonObject exceptionById(const QSqlDatabase& db, const QString& eventId)
{
    auto query = execute(db, QStringLiteral("SELECT ") + exceptionColumns() +
                                 QStringLiteral(" FROM charger_exceptions WHERE id=?"), {eventId});
    if (!query.next())
        throw AdminFailure("NOT_FOUND");
    return exceptionDto(query);
}
QJsonObject runtime(const QSqlDatabase& db, const QString& chargerId)
{
    auto query = execute(db, QStringLiteral(
        "SELECT c.status,o.id,o.telemetry_captured_at,o.telemetry_power_watts,o.energy_wh,"
        "o.duration_seconds,p.unit_price_cents_per_kwh FROM chargers c LEFT JOIN orders o "
        "ON o.charger_id=c.id AND o.status='CHARGING' LEFT JOIN order_pricing_snapshots p "
        "ON p.order_id=o.id WHERE c.id=?"), {chargerId});
    if (!query.next())
        throw AdminFailure("NOT_FOUND");
    QJsonObject result{{"chargerId", chargerId}, {"status", query.value(0).toString()},
                       {"sessionId", QJsonValue::Null}, {"capturedAt", QJsonValue::Null},
                       {"currentPowerWatts", QJsonValue::Null}, {"energyWh", QJsonValue::Null},
                       {"chargeSeconds", QJsonValue::Null}, {"currentAmountCents", QJsonValue::Null},
                       {"lastHeartbeatAt", QJsonValue::Null},
                       {"source", "SIMULATED_METER"}, {"simulated", true}};
    if (query.value(0).toString() != QStringLiteral("CHARGING") || query.value(1).isNull())
        return {{"item", result}};
    result.insert("sessionId", query.value(1).toString());
    // Refresh only reads the last sample produced by the charging service. It
    // must not label the read time (or charger.updated_at) as telemetry/heartbeat.
    if (query.value(2).isNull() || query.value(3).isNull())
        return {{"item", result}};
    const qint64 power = query.value(3).toLongLong();
    const qint64 energy = query.value(4).toLongLong();
    const qint64 seconds = query.value(5).toLongLong();
    if (power <= 0 || power > maximumInteger || energy < 0 || energy > maximumInteger ||
        seconds < 0 || seconds > maximumInteger)
        throw AdminFailure("DATABASE_ERROR");
    result.insert("capturedAt", query.value(2).toString());
    result.insert("currentPowerWatts", power);
    result.insert("energyWh", energy);
    result.insert("chargeSeconds", seconds);
    result.insert("estimated", true);
    if (!query.value(6).isNull()) {
        const qint64 price = query.value(6).toLongLong();
        if (price < 0 || price > maximumInteger)
            throw AdminFailure("DATABASE_ERROR");
        qint64 amount = 0;
        if (!calculateEnergyFeeCents(energy, price, &amount))
            throw AdminFailure("DATABASE_ERROR");
        result.insert("currentAmountCents", amount);
    }
    return {{"item", result}};
}
} // namespace

bool AdminChargerExtensions::handlesRead(const QString& action)
{
    return action == QStringLiteral("chargers.runtime.get") ||
           action == QStringLiteral("charger_exceptions.list") ||
           action == QStringLiteral("charger_exceptions.get");
}

void AdminChargerExtensions::validateRead(const QString& action, const QJsonObject& p)
{
    require(handlesRead(action));
    if (action != QStringLiteral("charger_exceptions.list")) {
        fields(p, {"id"});
        id(p, "id");
        return;
    }
    fields(p, {"page", "pageSize", "chargerId", "status", "severity", "occurredAtFrom", "occurredAtTo"});
    if (p.contains("page")) integer(p, "page", 1000000);
    if (p.contains("pageSize")) integer(p, "pageSize", 100);
    if (p.contains("chargerId")) id(p, "chargerId");
    if (p.contains("status"))
        require(p.value("status").isString() &&
                QStringList{"ACTIVE", "ACKNOWLEDGED", "RECOVERING", "RECOVERED"}.contains(p.value("status").toString()));
    if (p.contains("severity"))
        require(p.value("severity").isString() &&
                QStringList{"WARNING", "CRITICAL"}.contains(p.value("severity").toString()));
    for (const auto& key : {QStringLiteral("occurredAtFrom"), QStringLiteral("occurredAtTo")})
        if (p.contains(key)) timestamp(p, key);
    if (p.contains("occurredAtFrom") && p.contains("occurredAtTo"))
        require(p.value("occurredAtFrom").toString() < p.value("occurredAtTo").toString());
}

QJsonObject AdminChargerExtensions::read(const QSqlDatabase& db, const QString& action,
                                        const QJsonObject& p)
{
    validateRead(action, p);
    if (action == QStringLiteral("chargers.runtime.get"))
        return runtime(db, p.value("id").toString());
    if (action == QStringLiteral("charger_exceptions.get"))
        return {{"item", exceptionById(db, p.value("id").toString())}};
    QString where = QStringLiteral(" WHERE 1=1");
    QVariantList bindings;
    const QList<QPair<QString, QString>> filters{{"chargerId", "charger_id=?"}, {"status", "status=?"},
                                               {"severity", "severity=?"}, {"occurredAtFrom", "occurred_at>=?"},
                                               {"occurredAtTo", "occurred_at<?"}};
    for (const auto& filter : filters) {
        if (p.contains(filter.first)) {
            where += QStringLiteral(" AND ") + filter.second;
            bindings.append(p.value(filter.first).toString());
        }
    }
    const qint64 total = count(db, QStringLiteral("SELECT COUNT(*) FROM charger_exceptions") + where, bindings);
    const int page = p.value("page").toInt(1), size = p.value("pageSize").toInt(20);
    bindings << size << (page - 1) * size;
    auto query = execute(db, QStringLiteral("SELECT ") + exceptionColumns() +
                                 QStringLiteral(" FROM charger_exceptions") + where +
                                 QStringLiteral(" ORDER BY occurred_at DESC,id DESC LIMIT ? OFFSET ?"), bindings);
    QJsonArray items;
    while (query.next()) items.append(exceptionDto(query));
    return {{"items", items}, {"total", total}, {"page", page}, {"pageSize", size}};
}

QJsonValue AdminChargerExtensions::activeException(const QSqlDatabase& db, const QString& chargerId)
{
    auto query = execute(db, QStringLiteral("SELECT ") + exceptionColumns() +
        QStringLiteral(" FROM charger_exceptions WHERE charger_id=? AND status IN "
                       "('ACTIVE','ACKNOWLEDGED','RECOVERING') ORDER BY occurred_at DESC,id DESC LIMIT 1"), {chargerId});
    return query.next() ? QJsonValue(exceptionDto(query)) : QJsonValue(QJsonValue::Null);
}

void AdminChargerExtensions::recordStatusChange(const QSqlDatabase& db, const QString& chargerId,
                                                const QString& previous, const QString& target,
                                                const QString& now)
{
    Q_UNUSED(now);
    if (previous == target || (target != QStringLiteral("FAULT") && target != QStringLiteral("OFFLINE")))
        return;
    const bool fault = target == QStringLiteral("FAULT");
    // The charger version may be advanced beyond the clock to break a same-ms
    // optimistic-lock tie. An event has its own actual occurrence clock.
    const QString occurredAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    execute(db, QStringLiteral("INSERT INTO charger_exceptions(charger_id,code,severity,safe_summary,"
                               "status,occurred_at,recoverable,recovery_action,updated_at) "
                               "VALUES(?,?,?,?,'ACTIVE',?,1,'SIMULATE_RESTORE',?)"),
            {chargerId, fault ? QStringLiteral("SIMULATED_FAULT") : QStringLiteral("SIMULATED_OFFLINE"),
             fault ? QStringLiteral("CRITICAL") : QStringLiteral("WARNING"),
             fault ? QStringLiteral("管理员将电桩切换为模拟故障状态") : QStringLiteral("管理员将电桩切换为模拟离线状态"),
             occurredAt, occurredAt});
}

void AdminChargerExtensions::validateRecover(const QJsonObject& p)
{
    fields(p, {"id", "operationId", "expectedUpdatedAt", "recoveryAction"});
    id(p, "id");
    timestamp(p, "expectedUpdatedAt");
    require(p.value("operationId").isString() &&
            QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,64}$")).match(p.value("operationId").toString()).hasMatch());
    require(p.value("recoveryAction").toString() == QStringLiteral("SIMULATE_RESTORE"));
}

QJsonObject AdminChargerExtensions::recover(const QSqlDatabase& db, qint64 adminId,
                                           const QJsonObject& p, const QString& currentTime)
{
    validateRecover(p);
    const QString eventId = p.value("id").toString();
    const auto event = exceptionById(db, eventId);
    if (event.value("updatedAt") != p.value("expectedUpdatedAt"))
        throw AdminFailure("CONFLICT");
    if (!event.value("recoverable").toBool() ||
        !QStringList{"ACTIVE", "ACKNOWLEDGED"}.contains(event.value("status").toString()) ||
        event.value("recoveryAction") != p.value("recoveryAction"))
        throw AdminFailure("INVALID_STATE_TRANSITION");
    const QString chargerId = event.value("chargerId").toString();
    auto charger = execute(db, QStringLiteral("SELECT c.status,c.updated_at,s.status FROM chargers c "
                                              "JOIN stations s ON s.id=c.station_id WHERE c.id=?"), {chargerId});
    if (!charger.next()) throw AdminFailure("NOT_FOUND");
    const QString state = charger.value(0).toString(), oldCharger = charger.value(1).toString();
    if (state == QStringLiteral("RESERVED") || state == QStringLiteral("CHARGING") ||
        count(db, QStringLiteral("SELECT COUNT(*) FROM orders WHERE charger_id=? AND status IN ('RESERVED','CHARGING')"), {chargerId}) ||
        count(db, QStringLiteral("SELECT COUNT(*) FROM reservations WHERE charger_id=? AND status='ACTIVE'"), {chargerId}))
        throw AdminFailure("RESOURCE_BUSY");
    if (charger.value(2).toString() != QStringLiteral("ACTIVE"))
        throw AdminFailure("INVALID_STATE_TRANSITION");
    charger.finish();
    QString now = currentTime;
    for (const auto& old : {oldCharger, event.value("updatedAt").toString()}) {
        const auto parsed = QDateTime::fromString(old, Qt::ISODateWithMs);
        if (parsed.isValid() && now <= old)
            now = parsed.addMSecs(1).toUTC().toString(Qt::ISODateWithMs);
    }
    const bool others = count(db, QStringLiteral("SELECT COUNT(*) FROM charger_exceptions WHERE charger_id=? "
                                                 "AND id<>? AND status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING')"), {chargerId, eventId}) > 0;
    const QString command = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString message = others ? QStringLiteral("当前异常已完成受控模拟恢复；电桩仍有其他活动异常。未发送硬件命令。")
                                   : QStringLiteral("已完成受控模拟恢复并将电桩设为空闲；未发送硬件命令。请人工确认设备状态。") ;
    const auto updated = execute(db, QStringLiteral("UPDATE charger_exceptions SET status='RECOVERED',recovered_at=?,"
        "recovered_by_admin_id=?,recovery_command_id=?,recovery_message=?,recoverable=0,updated_at=? "
        "WHERE id=? AND updated_at=? AND status IN ('ACTIVE','ACKNOWLEDGED')"),
        {currentTime, adminId, command, message, now, eventId, p.value("expectedUpdatedAt").toString()});
    if (updated.numRowsAffected() != 1) throw AdminFailure("CONFLICT");
    if (!others) {
        const auto restored = execute(db, QStringLiteral("UPDATE chargers SET status='AVAILABLE',updated_at=? WHERE id=? AND updated_at=?"),
                                      {now, chargerId, oldCharger});
        if (restored.numRowsAffected() != 1) throw AdminFailure("CONFLICT");
    }
    return {{"item", exceptionById(db, eventId)}, {"commandId", command}, {"simulated", true}};
}
} // namespace charging::server
