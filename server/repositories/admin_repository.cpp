#include "admin_repository.h"

#include "dashboard_repository.h"
#include "admin_charger_extensions.h"
#include "admin_order_billing.h"
#include "charging_target_repository.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTimeZone>

namespace charging::server {
namespace {
QSqlQuery execute(const QSqlDatabase& db, const QString& sql, const QVariantList& values = {})
{
    QSqlQuery q(db);
    if (!q.prepare(sql))
        throw AdminFailure("DATABASE_ERROR");
    for (const auto& value : values)
        q.addBindValue(value);
    if (!q.exec()) {
        // SQLite drivers may return extended codes (e.g. UNIQUE=2067).
        const int code = q.lastError().nativeErrorCode().toInt() & 0xff;
        if (code == 19 || code == 5 || code == 6)
            throw AdminFailure("CONFLICT");
        throw AdminFailure("DATABASE_ERROR");
    }
    return q;
}
QJsonObject row(const QSqlQuery& q)
{
    QJsonObject result;
    const auto record = q.record();
    for (int i = 0; i < record.count(); ++i) {
        const QString name = record.fieldName(i);
        // SQLite's typed NULL text QVariant otherwise becomes an empty JSON
        // string on some Qt versions. Preserve unknown legacy data as null.
        if (q.value(i).isNull())
            result.insert(name, QJsonValue(QJsonValue::Null));
        else if (name == QStringLiteral("id") || name.endsWith(QStringLiteral("Id")))
            result.insert(name,
                          q.value(i).isNull() ? QJsonValue() : QJsonValue(q.value(i).toString()));
        else if (name == QStringLiteral("phone")) {
            const QString phone = q.value(i).toString();
            result.insert(name, phone.size() == 11
                                    ? phone.left(3) + QStringLiteral("****") + phone.right(4)
                                    : QStringLiteral("***"));
        } else
            result.insert(name, QJsonValue::fromVariant(q.value(i)));
    }
    return result;
}
qint64 scalar(const QSqlDatabase& db, const QString& sql, const QVariantList& values = {})
{
    auto q = execute(db, sql, values);
    if (!q.next())
        throw AdminFailure("DATABASE_ERROR");
    return q.value(0).toLongLong();
}
QString stamp(const QJsonObject& credentials)
{
    return credentials.value(QStringLiteral("password_algorithm")).toString() + QLatin1Char(':') +
           credentials.value(QStringLiteral("password_salt")).toString() + QLatin1Char(':') +
           credentials.value(QStringLiteral("password_hash")).toString();
}
QString literalLikePattern(QString value)
{
    value.replace(QLatin1Char('!'), QStringLiteral("!!"));
    value.replace(QLatin1Char('%'), QStringLiteral("!%"));
    value.replace(QLatin1Char('_'), QStringLiteral("!_"));
    return QLatin1Char('%') + value + QLatin1Char('%');
}
QJsonObject actionMetadata()
{
    // Fixed public vocabulary, not arbitrary action text extracted from audit
    // payloads. Historical actions remain filterable by their exact action.
    QJsonArray items;
    const auto append = [&items](const QString& action, const QString& label, const QString& category) {
        items.append(QJsonObject{{QStringLiteral("action"), action},
                                  {QStringLiteral("valueLabel"), label},
                                  {QStringLiteral("category"), category}});
    };
    append(QStringLiteral("station.create"), QStringLiteral("新增电站"), QStringLiteral("STATION"));
    append(QStringLiteral("station.edit"), QStringLiteral("编辑电站"), QStringLiteral("STATION"));
    append(QStringLiteral("station.status"), QStringLiteral("电站启停"), QStringLiteral("STATION"));
    append(QStringLiteral("user.status"), QStringLiteral("冻结或解冻用户"), QStringLiteral("USER"));
    append(QStringLiteral("charger.status"), QStringLiteral("电桩状态变更"), QStringLiteral("CHARGER"));
    append(QStringLiteral("charger.restart"), QStringLiteral("电桩模拟重启"), QStringLiteral("CHARGER"));
    append(QStringLiteral("charger_exceptions.recover"), QStringLiteral("异常模拟恢复"), QStringLiteral("EXCEPTION"));
    return {{QStringLiteral("items"), items}};
}
QJsonObject optionRows(const QSqlDatabase& db, const QString& action, const QJsonObject& p)
{
    QString select, from, search;
    if (action == QStringLiteral("stations.options")) {
        select = QStringLiteral("s.id,s.code,s.name");
        from = QStringLiteral("stations s");
        search = QStringLiteral("s.code LIKE ? ESCAPE '!' OR s.name LIKE ? ESCAPE '!'");
    } else if (action == QStringLiteral("chargers.options")) {
        select = QStringLiteral("s.id,s.code,s.code AS name,s.station_id AS stationId,t.name AS stationName");
        from = QStringLiteral("chargers s JOIN stations t ON t.id=s.station_id");
        search = QStringLiteral("s.code LIKE ? ESCAPE '!' OR t.name LIKE ? ESCAPE '!'");
    } else if (action == QStringLiteral("admins.options")) {
        select = QStringLiteral("s.id,s.username AS code,s.display_name AS name");
        from = QStringLiteral("admins s");
        search = QStringLiteral("s.username LIKE ? ESCAPE '!' OR s.display_name LIKE ? ESCAPE '!'");
    } else {
        throw AdminFailure("INVALID_ARGUMENT");
    }
    QString where = QStringLiteral(" WHERE 1=1");
    QVariantList bindings;
    if (p.contains(QStringLiteral("stationId"))) {
        where += QStringLiteral(" AND s.station_id=?");
        bindings << p.value(QStringLiteral("stationId")).toString();
    }
    if (!p.value(QStringLiteral("keyword")).toString().isEmpty()) {
        where += QStringLiteral(" AND (") + search + QLatin1Char(')');
        const QString pattern = literalLikePattern(p.value(QStringLiteral("keyword")).toString());
        bindings << pattern << pattern;
    }
    const qint64 total = scalar(db, QStringLiteral("SELECT COUNT(*) FROM ") + from + where, bindings);
    const int page = p.value(QStringLiteral("page")).toInt(1);
    const int pageSize = p.value(QStringLiteral("pageSize")).toInt(20);
    bindings << pageSize << (page - 1) * pageSize;
    auto query = execute(db, QStringLiteral("SELECT ") + select + QStringLiteral(" FROM ") + from +
                        where + QStringLiteral(" ORDER BY s.id ASC LIMIT ? OFFSET ?"), bindings);
    QJsonArray items;
    while (query.next())
        items.append(row(query));
    return {{QStringLiteral("items"), items}, {QStringLiteral("total"), total},
            {QStringLiteral("page"), page}, {QStringLiteral("pageSize"), pageSize}};
}
} // namespace

AdminRepository::AdminRepository(const QSqlDatabase& database) : database_(database) {}

QJsonObject AdminRepository::credentialQuery(const QString& predicate, const QVariant& value) const
{
    auto q = execute(database_,
                     QStringLiteral("SELECT id, username, display_name, password_algorithm, "
                                    "password_salt, password_hash, status FROM admins WHERE ") +
                         predicate,
                     {value});
    return q.next() ? row(q) : QJsonObject();
}
QJsonObject AdminRepository::credentials(const QString& username) const
{
    return credentialQuery(QStringLiteral("username = ? COLLATE NOCASE"), username);
}
QJsonObject AdminRepository::credentials(qint64 id) const
{
    return credentialQuery(QStringLiteral("id = ?"), id);
}
void AdminRepository::recordLogin(qint64 id) const
{
    execute(
        database_,
        QStringLiteral("UPDATE admins SET last_login_at = ? WHERE id = ? AND status = 'ACTIVE'"),
        {QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), id});
}

QJsonObject AdminRepository::read(const QString& entity, const QJsonObject& p) const
{
    execute(database_, QStringLiteral("BEGIN"));
    try {
        const auto result = AdminChargerExtensions::handlesRead(entity)
                                ? AdminChargerExtensions::read(database_, entity, p)
                                : entity == QStringLiteral("operation_logs.actions")
                                      ? actionMetadata()
                                      : entity.endsWith(QStringLiteral(".options"))
                                            ? optionRows(database_, entity, p)
                                            : readRows(entity, p);
        execute(database_, QStringLiteral("COMMIT"));
        return result;
    } catch (...) {
        auto db = database_;
        db.rollback();
        throw;
    }
}

QJsonObject AdminRepository::summary(const QString& entity, const QJsonObject& p,
                                     const QDateTime& now) const
{
    execute(database_, QStringLiteral("BEGIN"));
    try {
        const auto result = readRows(entity, p, true, now);
        execute(database_, QStringLiteral("COMMIT"));
        return result;
    } catch (...) {
        auto db = database_;
        db.rollback();
        throw;
    }
}

QJsonObject AdminRepository::readRows(const QString& entity, const QJsonObject& p, bool aggregate,
                                      const QDateTime& now) const
{
    QString select, from, search, statusColumn;
    if (entity == QStringLiteral("stations")) {
        select = QStringLiteral(
            "s.id,s.code,s.name,s.address,s.city,s.district,s.contact_name AS contactName,"
            "s.contact_phone AS contactPhone,s.latitude,s.longitude,s.price_cents_per_kwh AS "
            "priceCentsPerKwh,s.status,s.updated_at AS updatedAt,(SELECT COUNT(*) FROM chargers "
            "WHERE station_id=s.id) AS totalChargers,(SELECT COUNT(*) FROM chargers WHERE "
            "station_id=s.id AND status='AVAILABLE') AS availableChargers,(SELECT COUNT(*) "
            "FROM chargers WHERE station_id=s.id AND status!='OFFLINE') AS onlineChargerCount");
        from = QStringLiteral("stations s");
        search = QStringLiteral("(s.name LIKE ? ESCAPE '!' OR s.code LIKE ? ESCAPE '!' OR "
                                "s.address LIKE ? ESCAPE '!')");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("chargers")) {
        select = QStringLiteral("s.id,s.station_id AS stationId,s.code,s.type,s.power_watts AS "
                                "powerWatts,s.status,s.updated_at AS updatedAt,t.name AS "
                                "stationName,s.total_charge_count AS "
                                "totalChargeCount,s.total_charge_seconds AS totalChargeSeconds");
        from = QStringLiteral("chargers s JOIN stations t ON t.id=s.station_id");
        search = QStringLiteral("(s.code LIKE ? ESCAPE '!' OR t.name LIKE ? ESCAPE '!' OR "
                                "s.code LIKE ? ESCAPE '!')");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("users")) {
        select = QStringLiteral(
            "s.id,s.phone,s.nickname,s.avatar_key AS avatarKey,s.balance_cents AS "
            "balanceCents,s.status,s.created_at AS createdAt,s.created_at AS createdAtUtc,"
            "s.updated_at AS updatedAt,(SELECT COUNT(*) FROM orders WHERE "
            "user_id=s.id) AS orderCount,(SELECT COUNT(*) FROM orders WHERE user_id=s.id AND "
            "status IN ('RESERVED','CHARGING','WAITING_PAYMENT')) AS unfinishedOrderCount,(SELECT "
            "COUNT(*) FROM recharge_records WHERE user_id=s.id) AS rechargeCount");
        from = QStringLiteral("users s");
        search = QStringLiteral("(s.phone LIKE ? ESCAPE '!' OR s.nickname LIKE ? ESCAPE '!' OR "
                                "s.phone LIKE ? ESCAPE '!')");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("orders")) {
        select = QStringLiteral(
            "s.id,s.order_no AS orderNo,s.user_id AS userId,s.charger_id AS "
            "chargerId,s.reservation_id AS reservationId,s.status,s.unit_price_cents_per_kwh AS "
            "unitPriceCentsPerKwh,s.energy_wh AS energyWh,s.duration_seconds AS "
            "durationSeconds,s.amount_cents AS amountCents,s.created_at AS createdAt,s.started_at "
            "AS startedAt,s.stopped_at AS stoppedAt,s.paid_at AS paidAt,u.phone,u.nickname,c.code "
            "AS chargerCode,t.id AS stationId,t.name AS stationName");
        from = QStringLiteral("orders s JOIN users u ON u.id=s.user_id JOIN chargers c ON "
                              "c.id=s.charger_id JOIN stations t ON t.id=c.station_id");
        search = QStringLiteral("(s.order_no LIKE ? ESCAPE '!' OR u.phone LIKE ? ESCAPE '!' OR "
                                "t.name LIKE ? ESCAPE '!')");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("recharges")) {
        select = QStringLiteral(
            "s.id,s.transaction_no AS transactionNo,s.user_id AS userId,s.amount_cents AS "
            "amountCents,s.balance_after_cents AS balanceAfterCents,s.status,s.created_at AS "
            "createdAt,u.phone,u.nickname");
        from = QStringLiteral("recharge_records s JOIN users u ON u.id=s.user_id");
        search = QStringLiteral("(s.transaction_no LIKE ? ESCAPE '!' OR u.phone LIKE ? ESCAPE '!' "
                                "OR u.nickname LIKE ? ESCAPE '!')");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("operation_logs")) {
        // Public audit metadata only: arbitrary details_json may contain secrets
        // or replay payloads and is intentionally not exposed to the UI.
        select = QStringLiteral("s.id,s.admin_id AS adminId,s.action,s.target_type AS "
                                "targetType,s.target_id AS targetId,s.created_at AS createdAt");
        from = QStringLiteral("operation_logs s");
        search = QStringLiteral("(s.action LIKE ? ESCAPE '!' OR s.target_type LIKE ? ESCAPE '!' "
                                "OR s.target_id LIKE ? ESCAPE '!')");
    } else
        throw AdminFailure("INVALID_ARGUMENT");
    QString where = QStringLiteral(" WHERE 1=1");
    QVariantList bindings;
    if (p.contains(QStringLiteral("id"))) {
        where += QStringLiteral(" AND s.id=?");
        bindings << p.value(QStringLiteral("id")).toString();
    }
    const QString keyword = p.value(QStringLiteral("keyword")).toString().trimmed();
    if (!keyword.isEmpty()) {
        where += QStringLiteral(" AND ") + search;
        const QString pattern = literalLikePattern(keyword);
        bindings << pattern << pattern << pattern;
    }
    if (p.contains(QStringLiteral("status"))) {
        where += QStringLiteral(" AND ") + statusColumn + QStringLiteral("=?");
        bindings << p.value(QStringLiteral("status")).toString();
    }
    if (entity == QStringLiteral("stations")) {
        if (p.value(QStringLiteral("idleOnly")).toBool())
            where += QStringLiteral(" AND NOT EXISTS (SELECT 1 FROM chargers busy WHERE "
                                    "busy.station_id=s.id AND busy.status IN ('RESERVED','CHARGING'))");
        for (const auto& field : {QStringLiteral("city"), QStringLiteral("district")}) {
            if (p.contains(field)) {
                where += QStringLiteral(" AND s.") + field + QStringLiteral("=?");
                bindings << p.value(field).toString();
            }
        }
    }
    if (entity == QStringLiteral("chargers")) {
        const QList<QPair<QString, QString>> filters{
            {QStringLiteral("powerWatts"), QStringLiteral("=")},
            {QStringLiteral("minPowerWatts"), QStringLiteral(">=")},
            {QStringLiteral("maxPowerWatts"), QStringLiteral("<=")}};
        for (const auto& filter : filters) {
            if (p.contains(filter.first)) {
                where += QStringLiteral(" AND s.power_watts") + filter.second + QLatin1Char('?');
                bindings << p.value(filter.first).toVariant();
            }
        }
    }
    if (entity == QStringLiteral("users")) {
        for (const auto& filter : {qMakePair(QStringLiteral("minBalanceCents"), QStringLiteral(">=")),
                                   qMakePair(QStringLiteral("maxBalanceCents"), QStringLiteral("<="))}) {
            if (p.contains(filter.first)) {
                where += QStringLiteral(" AND s.balance_cents") + filter.second + QLatin1Char('?');
                bindings << p.value(filter.first).toVariant();
            }
        }
    }
    if (entity == QStringLiteral("chargers") && p.contains(QStringLiteral("stationId"))) {
        where += QStringLiteral(" AND s.station_id=?");
        bindings << p.value(QStringLiteral("stationId")).toString();
    }
    if (entity == QStringLiteral("chargers") && p.contains(QStringLiteral("type"))) {
        where += QStringLiteral(" AND s.type=?");
        bindings << p.value(QStringLiteral("type")).toString();
    }
    if (entity == QStringLiteral("chargers") && p.value(QStringLiteral("abnormalOnly")).toBool())
        where += QStringLiteral(" AND (s.status IN ('FAULT','OFFLINE') OR EXISTS "
                                "(SELECT 1 FROM charger_exceptions e WHERE e.charger_id=s.id "
                                "AND e.status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING')))");
    if (entity == QStringLiteral("orders")) {
        if (p.contains(QStringLiteral("orderNo"))) {
            where += QStringLiteral(" AND s.order_no=?");
            bindings << p.value(QStringLiteral("orderNo")).toString();
        }
        if (p.contains(QStringLiteral("userKeyword"))) {
            where += QStringLiteral(" AND u.nickname LIKE ? ESCAPE '!'");
            bindings << literalLikePattern(p.value(QStringLiteral("userKeyword")).toString());
        }
        if (p.contains(QStringLiteral("phone"))) {
            where += QStringLiteral(" AND u.phone=?");
            bindings << p.value(QStringLiteral("phone")).toString();
        }
        if (p.contains(QStringLiteral("stationId"))) {
            where += QStringLiteral(" AND c.station_id=?");
            bindings << p.value(QStringLiteral("stationId")).toString();
        }
        if (p.contains(QStringLiteral("chargerId"))) {
            where += QStringLiteral(" AND s.charger_id=?");
            bindings << p.value(QStringLiteral("chargerId")).toString();
        }
    }
    if (entity == QStringLiteral("orders") || entity == QStringLiteral("users") || entity == QStringLiteral("recharges") ||
        entity == QStringLiteral("operation_logs")) {
        if (p.contains(QStringLiteral("createdAtFrom"))) {
            where += QStringLiteral(" AND s.created_at>=?");
            bindings << p.value(QStringLiteral("createdAtFrom")).toString();
        }
        if (p.contains(QStringLiteral("createdAtTo"))) {
            where += QStringLiteral(" AND s.created_at<?");
            bindings << p.value(QStringLiteral("createdAtTo")).toString();
        }
    }
    if (entity == QStringLiteral("operation_logs")) {
        const QList<QPair<QString, QString>> filters{
            {QStringLiteral("adminId"), QStringLiteral("s.admin_id")},
            {QStringLiteral("action"), QStringLiteral("s.action")},
            {QStringLiteral("targetType"), QStringLiteral("s.target_type")},
            {QStringLiteral("targetId"), QStringLiteral("s.target_id")}};
        for (const auto& filter : filters) {
            if (p.contains(filter.first)) {
                where += QStringLiteral(" AND ") + filter.second + QStringLiteral("=?");
                bindings << p.value(filter.first).toString();
            }
        }
    }
    if ((entity == QStringLiteral("orders") || entity == QStringLiteral("recharges")) &&
        p.contains(QStringLiteral("userId"))) {
        where += QStringLiteral(" AND s.user_id=?");
        bindings << p.value(QStringLiteral("userId")).toString();
    }
    if (aggregate) {
        const QTimeZone zone("Asia/Shanghai");
        if (!zone.isValid() || !now.isValid())
            throw AdminFailure("DATABASE_ERROR");
        const auto date = now.toTimeZone(zone).date();
        const auto boundary = [&zone](const QDate& d) {
            return QDateTime(d, QTime(0, 0), zone).toUTC().toString(Qt::ISODateWithMs);
        };
        // Values are generated internally, never interpolated from request text.
        const QString today = boundary(date), tomorrow = boundary(date.addDays(1));
        const QString month = boundary(QDate(date.year(), date.month(), 1));
        const QString nextMonth = boundary(QDate(date.year(), date.month(), 1).addMonths(1));
        const auto between = [](const QString& column, const QString& a, const QString& b) {
            return column + QStringLiteral(">='") + a + QStringLiteral("' AND ") + column +
                   QStringLiteral("<'") + b + QStringLiteral("'");
        };
        const QString dayCreated = between("s.created_at", today, tomorrow);
        const QString monthCreated = between("s.created_at", month, nextMonth);
        const auto sum = [](const QString& expression, const QString& alias) {
            return QStringLiteral("COALESCE(SUM(") + expression + QStringLiteral("),0) AS ") +
                   alias;
        };
        QStringList columns;
        if (entity == "chargers") {
            columns = {"COUNT(*) AS totalChargers", sum("s.status!='OFFLINE'", "onlineChargers"),
                       sum("s.status='FAULT'", "faultChargers"),
                       sum("s.total_charge_count", "totalChargeCount")};
        } else if (entity == "stations") {
            columns = {
                "COUNT(*) AS totalStations", sum("s.status='ACTIVE'", "activeStations"),
                sum("(SELECT COUNT(*) FROM chargers c WHERE c.station_id=s.id)", "totalChargers"),
                sum("(SELECT COUNT(*) FROM chargers c WHERE c.station_id=s.id AND "
                    "c.status!='OFFLINE')",
                    "onlineChargers"),
                sum("(SELECT COUNT(*) FROM orders o JOIN chargers c ON c.id=o.charger_id WHERE "
                    "c.station_id=s.id AND " +
                        between("o.created_at", today, tomorrow) + ")",
                    "todayOrderCount")};
        } else if (entity == "users") {
            columns = {"COUNT(*) AS totalUsers", sum(dayCreated, "todayNewUsers"),
                       sum("s.status='FROZEN'", "frozenUsers"),
                       sum("s.balance_cents", "totalBalanceCents")};
        } else if (entity == "orders") {
            const QString paid = "s.status='COMPLETED' AND s.paid_at IS NOT NULL";
            columns = {
                sum(dayCreated, "todayOrderCount"),
                sum("CASE WHEN " + paid + " AND " + between("s.paid_at", today, tomorrow) +
                        " THEN s.amount_cents ELSE 0 END",
                    "todayRevenueCents"),
                sum("CASE WHEN " + paid + " AND " + between("s.paid_at", month, nextMonth) +
                        " THEN s.amount_cents ELSE 0 END",
                    "monthRevenueCents"),
                sum("CASE WHEN " + paid + " THEN s.amount_cents ELSE 0 END", "totalRevenueCents"),
                sum("s.status='CHARGING'", "chargingOrderCount"),
                sum("s.status='WAITING_PAYMENT'", "waitingPaymentOrderCount")};
        } else if (entity == "recharges") {
            columns = {sum(dayCreated, "todayCount"),
                       sum("CASE WHEN s.status='SUCCESS' AND " + dayCreated +
                               " THEN s.amount_cents ELSE 0 END",
                           "todayAmountCents"),
                       sum("s.status='FAILED' AND " + dayCreated, "failedCountToday"),
                       sum("CASE WHEN s.status='SUCCESS' AND " + monthCreated +
                               " THEN s.amount_cents ELSE 0 END",
                           "monthAmountCents")};
        } else if (entity == "operation_logs") {
            columns = {sum(dayCreated, "todayCount"), sum(monthCreated, "monthCount"),
                       sum("s.admin_id IS NOT NULL AND " + dayCreated, "adminInitiatedCountToday"),
                       sum("s.admin_id IS NULL AND " + dayCreated, "systemInitiatedCountToday")};
        } else
            throw AdminFailure("INVALID_ARGUMENT");
        auto query =
            execute(database_, "SELECT " + columns.join(',') + " FROM " + from + where, bindings);
        if (!query.next())
            throw AdminFailure("DATABASE_ERROR");
        QJsonObject result;
        for (int i = 0; i < query.record().count(); ++i) {
            bool valid = false;
            const qint64 value = query.value(i).toLongLong(&valid);
            if (!valid || value < 0 || value > 9007199254740991LL)
                throw AdminFailure("DATABASE_ERROR");
            result.insert(query.record().fieldName(i), value);
        }
        result.insert("timeZone", "Asia/Shanghai");
        result.insert("observedAt", now.toUTC().toString(Qt::ISODateWithMs));
        return result;
    }
    const qint64 total =
        scalar(database_, QStringLiteral("SELECT COUNT(*) FROM ") + from + where, bindings);
    const int page = p.value(QStringLiteral("page")).toInt(1),
              size = p.value(QStringLiteral("pageSize")).toInt(20);
    // Fixed expressions only. The ID tie-breaker makes timestamp paging stable.
    const QString sort = p.value(QStringLiteral("sort")).toString(QStringLiteral("idAsc"));
    QString orderBy;
    if (sort == QStringLiteral("idAsc"))
        orderBy = QStringLiteral("s.id ASC");
    else if (sort == QStringLiteral("idDesc"))
        orderBy = QStringLiteral("s.id DESC");
    else if (sort == QStringLiteral("createdAtDesc") &&
             (entity == QStringLiteral("orders") || entity == QStringLiteral("recharges") ||
              entity == QStringLiteral("operation_logs")))
        orderBy = QStringLiteral("s.created_at DESC,s.id DESC");
    else if (sort == QStringLiteral("updatedAtDesc") && entity == QStringLiteral("chargers"))
        orderBy = QStringLiteral("s.updated_at DESC,s.id DESC");
    else
        throw AdminFailure("INVALID_ARGUMENT");
    bindings << size << (page - 1) * size;
    auto q =
        execute(database_,
                QStringLiteral("SELECT ") + select + QStringLiteral(" FROM ") + from + where +
                    QStringLiteral(" ORDER BY ") + orderBy + QStringLiteral(" LIMIT ? OFFSET ?"),
                bindings);
    QJsonArray items;
    while (q.next()) {
        auto value = row(q);
        if (entity == QStringLiteral("stations")) {
            // Contact information may be absent on legacy rows. Full phone is
            // exposed only by authenticated station detail, never list/options.
            if (!p.contains(QStringLiteral("id")) && !value.value(QStringLiteral("contactPhone")).isNull()) {
                const auto phone = value.value(QStringLiteral("contactPhone")).toString();
                value.insert(QStringLiteral("contactPhone"), phone.size() == 11
                    ? phone.left(3) + QStringLiteral("****") + phone.right(4) : QStringLiteral("***"));
            }
            // Connectivity and availability are different concepts: a faulted
            // charger is still online. Share the global summary's non-OFFLINE
            // definition and compute both list/detail percentages here.
            bool totalOk = false, onlineOk = false;
            const qint64 totalChargers =
                value.value(QStringLiteral("totalChargers")).toVariant().toLongLong(&totalOk);
            const qint64 onlineChargers =
                value.value(QStringLiteral("onlineChargerCount")).toVariant().toLongLong(&onlineOk);
            if (!totalOk || !onlineOk || totalChargers < 0 || onlineChargers < 0 ||
                onlineChargers > totalChargers || totalChargers > 9007199254740991LL)
                throw AdminFailure("DATABASE_ERROR");
            value.insert(QStringLiteral("onlineRatePercent"),
                         totalChargers == 0 ? 0.0
                                            : double(onlineChargers) / double(totalChargers) * 100.0);
        }
        if (entity == QStringLiteral("chargers")) {
            const bool maintenance = scalar(database_, QStringLiteral(
                "SELECT COUNT(*) FROM repair_reports WHERE charger_id=? AND status IN ('ACCEPTED','PROCESSING')"),
                {value.value(QStringLiteral("id")).toString()}) > 0;
            value.insert(QStringLiteral("maintenance"), maintenance);
            value.insert(QStringLiteral("displayStatus"), maintenance ? QStringLiteral("维护中")
                                                                        : value.value(QStringLiteral("status")).toString());
            value.insert(QStringLiteral("activeException"), AdminChargerExtensions::activeException(
                database_, value.value(QStringLiteral("id")).toString()));
            const QString state = value.value(QStringLiteral("status")).toString();
            // State classification, NOT a hardware fault diagnosis or event time.
            value.insert(QStringLiteral("exceptionType"),
                         state == QStringLiteral("FAULT") || state == QStringLiteral("OFFLINE")
                             ? QJsonValue(state)
                             : QJsonValue(QJsonValue::Null));
        }
        if (entity == QStringLiteral("orders")) {
            QJsonObject target;
            QString stopReason;
            if (!chargingTargetDto(database_, value.value(QStringLiteral("id")).toString().toLongLong(),
                                   &target, &stopReason))
                throw AdminFailure("DATABASE_ERROR");
            value.insert(QStringLiteral("target"), target.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(target));
            value.insert(QStringLiteral("stopReason"), stopReason.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(stopReason));
        }
        if (entity == QStringLiteral("orders") && p.contains(QStringLiteral("id"))) {
            QJsonObject billing;
            if (!orderBillingDto(database_, value.value(QStringLiteral("id")).toString().toLongLong(), &billing))
                throw AdminFailure("DATABASE_ERROR");
            for (auto it = billing.constBegin(); it != billing.constEnd(); ++it)
                value.insert(it.key(), it.value());
        }
        if (entity == QStringLiteral("recharges")) {
            const auto transaction = value.value(QStringLiteral("transactionNo")).toString();
            value.insert(QStringLiteral("transactionNo"), transaction.size() > 8
                ? transaction.left(4) + QStringLiteral("****") + transaction.right(4)
                : QStringLiteral("****"));
        }
        items.append(value);
    }
    if (p.contains(QStringLiteral("id"))) {
        if (items.isEmpty())
            throw AdminFailure("NOT_FOUND");
        return {{QStringLiteral("item"), items.first()}};
    }
    return {{QStringLiteral("items"), items},
            {QStringLiteral("total"), total},
            {QStringLiteral("page"), page},
            {QStringLiteral("pageSize"), size}};
}

QJsonObject AdminRepository::dashboard(int days, const QDateTime& now) const
{
    if ((days != 7 && days != 30) || !now.isValid())
        throw AdminFailure("INVALID_ARGUMENT");
    // UTC timestamps on the wire, Shanghai calendar boundaries for management.
    execute(database_, QStringLiteral("BEGIN"));
    try {
        DashboardRepository repo(database_);
        const auto summary = repo.summary(now);
        if (!summary.ok)
            throw AdminFailure("DATABASE_ERROR");
        const auto& s = summary.summary;
        QJsonObject data{{QStringLiteral("totalUsers"), s.totalUsers},
                         {QStringLiteral("activeStations"), s.activeStations},
                         {QStringLiteral("totalChargers"), s.totalChargers},
                         {QStringLiteral("availableChargers"), s.availableChargers},
                         {QStringLiteral("reservedChargers"), s.reservedChargers},
                         {QStringLiteral("chargingChargers"), s.chargingChargers},
                         {QStringLiteral("faultChargers"), s.faultChargers},
                         {QStringLiteral("offlineChargers"), s.offlineChargers},
                         {QStringLiteral("activeOrders"), s.activeOrders},
                         {QStringLiteral("todayRevenueCents"), s.todayRevenueCents},
                         {QStringLiteral("monthRevenueCents"), s.monthRevenueCents},
                         {QStringLiteral("timeZone"), QStringLiteral("Asia/Shanghai")},
                         {QStringLiteral("observedAt"), now.toString(Qt::ISODateWithMs)},
                         {QStringLiteral("onlineRatio"),
                          s.totalChargers
                              ? double(s.totalChargers - s.offlineChargers) / s.totalChargers
                              : 0.0}};
        QJsonArray points;
        const auto revenues = readRows("orders", {}, true, now);
        for (const auto& key : {"todayRevenueCents", "monthRevenueCents", "totalRevenueCents"})
            data.insert(key, revenues.value(key));
        const QTimeZone zone("Asia/Shanghai");
        const auto today = now.toTimeZone(zone).date();
        for (int i = days - 1; i >= 0; --i) {
            const auto date = today.addDays(-i);
            const QString start =
                QDateTime(date, QTime(0, 0), zone).toUTC().toString(Qt::ISODateWithMs);
            const QString end =
                QDateTime(date.addDays(1), QTime(0, 0), zone).toUTC().toString(Qt::ISODateWithMs);
            auto q =
                execute(database_,
                        QStringLiteral("SELECT COUNT(*), COALESCE(SUM(amount_cents),0) FROM orders "
                                       "WHERE status='COMPLETED' AND paid_at>=? AND paid_at<?"),
                        {start, end});
            if (!q.next())
                throw AdminFailure("DATABASE_ERROR");
            const qint64 amount = q.value(1).toLongLong();
            if (amount < 0 || amount > 9007199254740991LL)
                throw AdminFailure("DATABASE_ERROR");
            points.append(QJsonObject{{"date", date.toString(Qt::ISODate)},
                                      {"completedOrderCount", q.value(0).toLongLong()},
                                      {"revenueCents", amount}});
        }
        data.insert(QStringLiteral("trend"), points);
        // Reuse exactly the management-list DTOs and filters in the same read
        // transaction as the aggregates. No nested transaction or Mock source.
        data.insert(QStringLiteral("abnormalChargers"),
                    readRows(QStringLiteral("chargers"),
                             {{QStringLiteral("abnormalOnly"), true},
                              {QStringLiteral("sort"), QStringLiteral("updatedAtDesc")},
                              {QStringLiteral("pageSize"), 5}}));
        data.insert(QStringLiteral("latestOrders"),
                    readRows(QStringLiteral("orders"),
                             {{QStringLiteral("sort"), QStringLiteral("createdAtDesc")},
                              {QStringLiteral("pageSize"), 5}}));
        execute(database_, QStringLiteral("COMMIT"));
        return data;
    } catch (...) {
        auto db = database_;
        db.rollback();
        throw;
    }
}

QJsonObject AdminRepository::mutate(qint64 adminId, const QString& credentialStamp,
                                    const QString& action, const QJsonObject& p) const
{
    execute(database_, QStringLiteral("BEGIN IMMEDIATE"));
    try {
        const auto admin = credentials(adminId);
        if (admin.value(QStringLiteral("status")).toString() != QStringLiteral("ACTIVE") ||
            stamp(admin) != credentialStamp)
            throw AdminFailure("UNAUTHORIZED");
        const QString requestId = p.value(QStringLiteral("operationId")).toString();
        const QString fingerprint = QString::fromLatin1(
            QCryptographicHash::hash(action.toUtf8() +
                                         QJsonDocument(p).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256)
                .toHex());
        auto previous =
            execute(database_,
                    QStringLiteral("SELECT details_json FROM operation_logs WHERE admin_id=? AND "
                                   "target_type='ADMIN_COMMAND' AND target_id=?"),
                    {adminId, requestId});
        if (previous.next()) {
            const auto saved = QJsonDocument::fromJson(previous.value(0).toByteArray()).object();
            if (saved.value(QStringLiteral("fingerprint")).toString() != fingerprint)
                throw AdminFailure("CONFLICT");
            auto result = saved.value(QStringLiteral("result")).toObject();
            result.insert(QStringLiteral("idempotent"), true);
            previous.finish();
            execute(database_, QStringLiteral("COMMIT"));
            return result;
        }
        previous.finish();
        QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QString id = p.value(QStringLiteral("id")).toString();
        QJsonObject result;
        if (action == QStringLiteral("charger_exceptions.recover")) {
            result = AdminChargerExtensions::recover(database_, adminId, p, now);
        } else if (action == QStringLiteral("station.create")) {
            auto q =
                execute(database_,
                        QStringLiteral("INSERT INTO "
                                       "stations(code,name,address,latitude,longitude,price_cents_"
                                       "per_kwh,city,district,contact_name,contact_phone,status,updated_at) "
                                       "VALUES(?,?,?,?,?,?,?,?,?,?,'ACTIVE',?)"),
                        {p.value(QStringLiteral("code")).toString(),
                         p.value(QStringLiteral("name")).toString(),
                         p.value(QStringLiteral("address")).toString(),
                         p.value(QStringLiteral("latitude")).toDouble(),
                         p.value(QStringLiteral("longitude")).toDouble(),
                         p.value(QStringLiteral("priceCentsPerKwh")).toVariant(),
                         p.value(QStringLiteral("city")).toVariant(),
                         p.value(QStringLiteral("district")).toVariant(),
                         p.value(QStringLiteral("contactName")).toVariant(),
                         p.value(QStringLiteral("contactPhone")).toVariant(), now});
            id = q.lastInsertId().toString();
            const auto chargers = p.value(QStringLiteral("chargers")).toArray();
            for (const auto& value : chargers) {
                const auto c = value.toObject();
                execute(
                    database_,
                    QStringLiteral(
                        "INSERT INTO chargers(station_id,code,type,power_watts) VALUES(?,?,?,?)"),
                    {id, c.value(QStringLiteral("code")).toString(),
                     c.value(QStringLiteral("type")).toString(),
                     c.value(QStringLiteral("powerWatts")).toInt()});
            }
            result = readRows(QStringLiteral("stations"), {{QStringLiteral("id"), id}});
        } else {
            const QString entity =
                action.startsWith(QStringLiteral("station.")) ? QStringLiteral("stations")
                : action.startsWith(QStringLiteral("user."))  ? QStringLiteral("users")
                                                              : QStringLiteral("chargers");
            const auto current = readRows(entity, {{QStringLiteral("id"), id}})
                                     .value(QStringLiteral("item"))
                                     .toObject();
            const QString old = current.value(QStringLiteral("updatedAt")).toString();
            if (old != p.value(QStringLiteral("expectedUpdatedAt")).toString())
                throw AdminFailure("CONFLICT");
            const auto oldTime = QDateTime::fromString(old, Qt::ISODateWithMs);
            if (oldTime.isValid() && now <= old)
                now = oldTime.addMSecs(1).toUTC().toString(Qt::ISODateWithMs);
            QString update;
            QVariantList values;
            if (action == QStringLiteral("station.edit")) {
                update =
                    QStringLiteral("name=?,address=?,latitude=?,longitude=?,price_cents_per_kwh=?");
                values = {p.value(QStringLiteral("name")).toString(),
                          p.value(QStringLiteral("address")).toString(),
                          p.value(QStringLiteral("latitude")).toDouble(),
                          p.value(QStringLiteral("longitude")).toDouble(),
                          p.value(QStringLiteral("priceCentsPerKwh")).toVariant()};
                const QList<QPair<QString, QString>> contacts{
                    {QStringLiteral("city"), QStringLiteral("city")},
                    {QStringLiteral("district"), QStringLiteral("district")},
                    {QStringLiteral("contactName"), QStringLiteral("contact_name")},
                    {QStringLiteral("contactPhone"), QStringLiteral("contact_phone")}};
                for (const auto& contact : contacts) {
                    if (p.contains(contact.first)) {
                        update += QLatin1Char(',') + contact.second + QStringLiteral("=?");
                        values << p.value(contact.first).toString();
                    }
                }
            } else {
                QString target = p.value(QStringLiteral("status")).toString();
                if (action == QStringLiteral("charger.restart"))
                    target = QStringLiteral("AVAILABLE");
                if (entity == QStringLiteral("stations") && target == QStringLiteral("INACTIVE")) {
                    if (scalar(database_,
                               QStringLiteral("SELECT COUNT(*) FROM orders o JOIN chargers c ON "
                                              "c.id=o.charger_id WHERE c.station_id=? AND o.status "
                                              "IN ('RESERVED','CHARGING')"),
                               {id}) ||
                        scalar(database_,
                               QStringLiteral("SELECT COUNT(*) FROM chargers WHERE station_id=? "
                                              "AND status IN ('RESERVED','CHARGING')"),
                               {id}) ||
                        scalar(database_,
                               QStringLiteral(
                                   "SELECT COUNT(*) FROM reservations r JOIN chargers c ON "
                                   "c.id=r.charger_id WHERE c.station_id=? AND r.status='ACTIVE'"),
                               {id}))
                        throw AdminFailure("RESOURCE_BUSY");
                }
                if (entity == QStringLiteral("users") && target == QStringLiteral("FROZEN") &&
                    (scalar(database_,
                            QStringLiteral("SELECT COUNT(*) FROM orders WHERE user_id=? AND status "
                                           "IN ('RESERVED','CHARGING','WAITING_PAYMENT')"),
                            {id}) ||
                     scalar(database_,
                            QStringLiteral("SELECT COUNT(*) FROM reservations WHERE user_id=? AND "
                                           "status='ACTIVE'"),
                            {id})))
                    throw AdminFailure("RESOURCE_BUSY");
                if (entity == QStringLiteral("chargers")) {
                    // Only the repair workflow may restore a maintenance hold.
                    // Legacy status/restart controls must not bypass acceptance.
                    if (current.value(QStringLiteral("maintenance")).toBool())
                        throw AdminFailure("RESOURCE_BUSY");
                    const auto state = current.value(QStringLiteral("status")).toString();
                    if (state == QStringLiteral("CHARGING") ||
                        state == QStringLiteral("RESERVED") ||
                        scalar(database_,
                               QStringLiteral("SELECT COUNT(*) FROM orders WHERE charger_id=? AND "
                                              "status IN ('RESERVED','CHARGING')"),
                               {id}) ||
                        scalar(database_,
                               QStringLiteral("SELECT COUNT(*) FROM reservations WHERE "
                                              "charger_id=? AND status='ACTIVE'"),
                               {id}))
                        throw AdminFailure("RESOURCE_BUSY");
                    // Restart is a state simulation, not an exception recovery
                    // command. It must not make an unresolved fault reservable.
                    if (action == QStringLiteral("charger.restart") &&
                        current.value(QStringLiteral("activeException")).isObject())
                        throw AdminFailure("INVALID_STATE_TRANSITION");
                    if (target == QStringLiteral("AVAILABLE") &&
                        !scalar(database_,
                                QStringLiteral(
                                    "SELECT COUNT(*) FROM stations WHERE id=? AND status='ACTIVE'"),
                                {current.value(QStringLiteral("stationId")).toString()}))
                        throw AdminFailure("INVALID_STATE_TRANSITION");
                }
                update = QStringLiteral("status=?");
                values = {target};
            }
            values << now << id << old;
            const auto updated =
                execute(database_,
                        QStringLiteral("UPDATE ") + entity + QStringLiteral(" SET ") + update +
                            QStringLiteral(",updated_at=? WHERE id=? AND updated_at=?"),
                        values);
            if (updated.numRowsAffected() != 1)
                throw AdminFailure("CONFLICT");
            if (entity == QStringLiteral("chargers"))
                AdminChargerExtensions::recordStatusChange(
                    database_, id, current.value(QStringLiteral("status")).toString(),
                    action == QStringLiteral("charger.restart") ? QStringLiteral("AVAILABLE")
                                                                  : p.value(QStringLiteral("status")).toString(),
                    now);
            result = readRows(entity, {{QStringLiteral("id"), id}});
            if (action == QStringLiteral("charger.restart"))
                result.insert(QStringLiteral("simulated"), true);
        }
        result.insert(QStringLiteral("idempotent"), false);
        const QJsonObject details{{QStringLiteral("fingerprint"), fingerprint},
                                  {QStringLiteral("targetId"), id},
                                  {QStringLiteral("result"), result}};
        execute(database_,
                QStringLiteral("INSERT INTO "
                               "operation_logs(admin_id,action,target_type,target_id,details_json,"
                               "created_at) VALUES(?,?,'ADMIN_COMMAND',?,?,?)"),
                {adminId, action, requestId,
                 QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact)), now});
        execute(database_, QStringLiteral("COMMIT"));
        return result;
    } catch (...) {
        auto db = database_;
        db.rollback();
        throw;
    }
}
} // namespace charging::server
