#include "admin_repository.h"

#include "dashboard_repository.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

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
        if (name == QStringLiteral("id") || name.endsWith(QStringLiteral("Id")))
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
        const auto result = readRows(entity, p);
        execute(database_, QStringLiteral("COMMIT"));
        return result;
    } catch (...) {
        auto db = database_;
        db.rollback();
        throw;
    }
}

QJsonObject AdminRepository::readRows(const QString& entity, const QJsonObject& p) const
{
    QString select, from, search, statusColumn;
    if (entity == QStringLiteral("stations")) {
        select = QStringLiteral(
            "s.id,s.code,s.name,s.address,s.latitude,s.longitude,s.price_cents_per_kwh AS "
            "priceCentsPerKwh,s.status,s.updated_at AS updatedAt,(SELECT COUNT(*) FROM chargers "
            "WHERE station_id=s.id) AS totalChargers,(SELECT COUNT(*) FROM chargers WHERE "
            "station_id=s.id AND status='AVAILABLE') AS availableChargers");
        from = QStringLiteral("stations s");
        search = QStringLiteral("(s.name LIKE ? OR s.code LIKE ? OR s.address LIKE ?)");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("chargers")) {
        select = QStringLiteral("s.id,s.station_id AS stationId,s.code,s.type,s.power_watts AS "
                                "powerWatts,s.status,s.updated_at AS updatedAt,t.name AS "
                                "stationName,s.total_charge_count AS "
                                "totalChargeCount,s.total_charge_seconds AS totalChargeSeconds");
        from = QStringLiteral("chargers s JOIN stations t ON t.id=s.station_id");
        search = QStringLiteral("(s.code LIKE ? OR t.name LIKE ? OR s.code LIKE ?)");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("users")) {
        select = QStringLiteral(
            "s.id,s.phone,s.nickname,s.avatar_key AS avatarKey,s.balance_cents AS "
            "balanceCents,s.status,s.updated_at AS updatedAt,(SELECT COUNT(*) FROM orders WHERE "
            "user_id=s.id) AS orderCount,(SELECT COUNT(*) FROM orders WHERE user_id=s.id AND "
            "status IN ('RESERVED','CHARGING','WAITING_PAYMENT')) AS unfinishedOrderCount,(SELECT "
            "COUNT(*) FROM recharge_records WHERE user_id=s.id) AS rechargeCount");
        from = QStringLiteral("users s");
        search = QStringLiteral("(s.phone LIKE ? OR s.nickname LIKE ? OR s.phone LIKE ?)");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("orders")) {
        select = QStringLiteral(
            "s.id,s.order_no AS orderNo,s.user_id AS userId,s.charger_id AS "
            "chargerId,s.reservation_id AS reservationId,s.status,s.unit_price_cents_per_kwh AS "
            "unitPriceCentsPerKwh,s.energy_wh AS energyWh,s.duration_seconds AS "
            "durationSeconds,s.amount_cents AS amountCents,s.created_at AS createdAt,s.started_at "
            "AS startedAt,s.stopped_at AS stoppedAt,s.paid_at AS paidAt,u.phone,u.nickname,c.code "
            "AS chargerCode,t.name AS stationName");
        from = QStringLiteral("orders s JOIN users u ON u.id=s.user_id JOIN chargers c ON "
                              "c.id=s.charger_id JOIN stations t ON t.id=c.station_id");
        search = QStringLiteral("(s.order_no LIKE ? OR u.phone LIKE ? OR t.name LIKE ?)");
        statusColumn = QStringLiteral("s.status");
    } else if (entity == QStringLiteral("recharges")) {
        select = QStringLiteral(
            "s.id,s.transaction_no AS transactionNo,s.user_id AS userId,s.amount_cents AS "
            "amountCents,s.balance_after_cents AS balanceAfterCents,s.status,s.created_at AS "
            "createdAt,u.phone,u.nickname");
        from = QStringLiteral("recharge_records s JOIN users u ON u.id=s.user_id");
        search = QStringLiteral("(s.transaction_no LIKE ? OR u.phone LIKE ? OR u.nickname LIKE ?)");
        statusColumn = QStringLiteral("s.status");
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
        const QString pattern = QLatin1Char('%') + keyword + QLatin1Char('%');
        bindings << pattern << pattern << pattern;
    }
    if (p.contains(QStringLiteral("status"))) {
        where += QStringLiteral(" AND ") + statusColumn + QStringLiteral("=?");
        bindings << p.value(QStringLiteral("status")).toString();
    }
    if (entity == QStringLiteral("chargers") && p.contains(QStringLiteral("stationId"))) {
        where += QStringLiteral(" AND s.station_id=?");
        bindings << p.value(QStringLiteral("stationId")).toString();
    }
    if (entity == QStringLiteral("chargers") && p.contains(QStringLiteral("type"))) {
        where += QStringLiteral(" AND s.type=?");
        bindings << p.value(QStringLiteral("type")).toString();
    }
    if ((entity == QStringLiteral("orders") || entity == QStringLiteral("recharges")) &&
        p.contains(QStringLiteral("userId"))) {
        where += QStringLiteral(" AND s.user_id=?");
        bindings << p.value(QStringLiteral("userId")).toString();
    }
    const qint64 total =
        scalar(database_, QStringLiteral("SELECT COUNT(*) FROM ") + from + where, bindings);
    const int page = p.value(QStringLiteral("page")).toInt(1),
              size = p.value(QStringLiteral("pageSize")).toInt(20);
    // Only these two fixed sort expressions can reach SQL, even if called internally.
    const QString direction = p.value(QStringLiteral("sort")).toString() == QStringLiteral("idDesc")
                                  ? QStringLiteral(" DESC")
                                  : QStringLiteral(" ASC");
    bindings << size << (page - 1) * size;
    auto q = execute(database_,
                     QStringLiteral("SELECT ") + select + QStringLiteral(" FROM ") + from + where +
                         QStringLiteral(" ORDER BY s.id") + direction +
                         QStringLiteral(" LIMIT ? OFFSET ?"),
                     bindings);
    QJsonArray items;
    while (q.next())
        items.append(row(q));
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

QJsonObject AdminRepository::dashboard(int days) const
{
    // Match the existing repository contract: calendar boundaries are UTC,
    // paid COMPLETED orders only, recharges are not operating revenue.
    execute(database_, QStringLiteral("BEGIN"));
    try {
        DashboardRepository repo(database_);
        const auto now = QDateTime::currentDateTimeUtc();
        const auto summary = repo.summary(now);
        const auto trend = repo.revenueTrend(now.date().addDays(1 - days), now.date());
        if (!summary.ok || !trend.ok)
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
                         {QStringLiteral("timeZone"), QStringLiteral("UTC")},
                         {QStringLiteral("observedAt"), now.toString(Qt::ISODateWithMs)},
                         {QStringLiteral("onlineRatio"),
                          s.totalChargers
                              ? double(s.totalChargers - s.offlineChargers) / s.totalChargers
                              : 0.0}};
        QJsonArray points;
        for (const auto& v : trend.points)
            points.append(
                QJsonObject{{QStringLiteral("date"), v.date.toString(Qt::ISODate)},
                            {QStringLiteral("completedOrderCount"), v.completedOrderCount},
                            {QStringLiteral("revenueCents"), v.revenueCents}});
        data.insert(QStringLiteral("trend"), points);
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
        if (action == QStringLiteral("station.create")) {
            auto q =
                execute(database_,
                        QStringLiteral("INSERT INTO "
                                       "stations(code,name,address,latitude,longitude,price_cents_"
                                       "per_kwh,status,updated_at) VALUES(?,?,?,?,?,?,'ACTIVE',?)"),
                        {p.value(QStringLiteral("code")).toString(),
                         p.value(QStringLiteral("name")).toString(),
                         p.value(QStringLiteral("address")).toString(),
                         p.value(QStringLiteral("latitude")).toDouble(),
                         p.value(QStringLiteral("longitude")).toDouble(),
                         p.value(QStringLiteral("priceCentsPerKwh")).toVariant(), now});
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
