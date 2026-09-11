#include "queue_repository.h"
#include "charging_repository.h"
#include "repository_row_mapper.h"
#include "charging/common/model/model_json.h"

#include <QJsonArray>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace charging::server {
namespace {
QString utc(const QDateTime& value) { return value.toUTC().toString(Qt::ISODateWithMs); }
QJsonObject fail(const QString& code, const QString& message)
{
    return {{"success", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
QJsonObject dbFail() { return fail("DATABASE_ERROR", QStringLiteral("排队处理失败，请稍后重试")); }
QJsonObject success(const QJsonObject& data) { return {{"success", true}, {"data", data}}; }
class Transaction final
{
public:
    explicit Transaction(const QSqlDatabase& db) : db_(db) {}
    ~Transaction() { if (active_) { QSqlQuery q(db_); q.exec("ROLLBACK"); } }
    bool begin(QString* error = nullptr)
    {
        QSqlQuery q(db_); active_ = q.exec("BEGIN IMMEDIATE");
        if (!active_ && error) *error = q.lastError().text();
        return active_;
    }
    bool commit(QString* error = nullptr)
    {
        QSqlQuery q(db_);
        if (!q.exec("COMMIT")) { if (error) *error = q.lastError().text(); return false; }
        active_ = false; return true;
    }
private:
    QSqlDatabase db_;
    bool active_ = false;
};

const QString kColumns = QStringLiteral(
    "q.id,q.user_id,q.charger_id,c.station_id,q.status,q.entered_at,q.called_at,"
    "q.call_expires_at,q.ended_at,q.reservation_id,q.updated_at,c.code,s.name,c.status,"
    "u.nickname,u.phone,"
    "(SELECT count(*) FROM queue_entries p WHERE p.charger_id=q.charger_id "
    "AND p.status IN ('WAITING','CALLED') "
    "AND (p.entered_at<q.entered_at OR (p.entered_at=q.entered_at AND p.id<q.id))),"
    "EXISTS(SELECT 1 FROM repair_reports rp WHERE rp.charger_id=c.id "
    "AND rp.status IN ('ACCEPTED','PROCESSING'))");
const QString kFrom = QStringLiteral(
    " FROM queue_entries q JOIN chargers c ON c.id=q.charger_id "
    "JOIN stations s ON s.id=c.station_id JOIN users u ON u.id=q.user_id ");

QJsonValue nullableText(const QVariant& value)
{
    return value.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(value.toString());
}
QJsonObject row(const QSqlQuery& q, const QDateTime& now, bool admin)
{
    const QString status = q.value(4).toString();
    const bool active = status == "WAITING" || status == "CALLED";
    const int ahead = q.value(16).toInt();
    const auto expires = QDateTime::fromString(q.value(7).toString(), Qt::ISODateWithMs);
    QJsonObject item{{"id", QString::number(q.value(0).toLongLong())},
                     {"chargerId", QString::number(q.value(2).toLongLong())},
                     {"stationId", QString::number(q.value(3).toLongLong())},
                     {"status", status}, {"enteredAt", q.value(5).toString()},
                     {"calledAt", nullableText(q.value(6))},
                     {"callExpiresAt", nullableText(q.value(7))},
                     {"endedAt", nullableText(q.value(8))},
                     {"reservationId", q.value(9).isNull() ? QJsonValue(QJsonValue::Null)
                                                          : QJsonValue(QString::number(q.value(9).toLongLong()))},
                     {"updatedAt", q.value(10).toString()},
                     {"chargerCode", q.value(11).toString()}, {"stationName", q.value(12).toString()},
                     {"chargerStatus", q.value(13).toString()},
                     {"position", active ? QJsonValue(ahead + 1) : QJsonValue(QJsonValue::Null)},
                     {"aheadCount", active ? QJsonValue(ahead) : QJsonValue(QJsonValue::Null)},
                     {"confirmationSecondsRemaining", status == "CALLED" && expires.isValid()
                         ? QJsonValue(qMax<qint64>(0, (now.msecsTo(expires) + 999) / 1000))
                         : QJsonValue(QJsonValue::Null)},
                     {"maintenance", q.value(17).toBool()}};
    if (admin) {
        QString phone = q.value(15).toString();
        if (phone.size() == 11) phone.replace(3, 4, QStringLiteral("****"));
        else phone = QStringLiteral("—");
        item.insert("userId", QString::number(q.value(1).toLongLong()));
        item.insert("userNickname", q.value(14).toString());
        item.insert("userPhoneMasked", phone);
    }
    return item;
}
bool readItem(const QSqlDatabase& db, qint64 id, const QDateTime& now, QJsonObject* out)
{
    QSqlQuery q(db);
    q.prepare("SELECT " + kColumns + kFrom + " WHERE q.id=?");
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return false;
    *out = row(q, now, false); return true;
}
QJsonObject itemReply(const QSqlDatabase& db, qint64 id, const QDateTime& now, bool idempotent)
{
    QJsonObject item;
    if (!readItem(db, id, now, &item)) return dbFail();
    return success({{"item", item}, {"serverNow", utc(now)}, {"idempotent", idempotent}});
}
bool hasActiveUser(const QSqlDatabase& db, qint64 id, QJsonObject* error)
{
    QSqlQuery q(db); q.prepare("SELECT status FROM users WHERE id=?"); q.addBindValue(id);
    if (!q.exec()) { *error = dbFail(); return false; }
    if (!q.next()) { *error = fail("UNAUTHORIZED", QStringLiteral("请重新登录")); return false; }
    if (q.value(0).toString() != "ACTIVE") {
        *error = fail("USER_FROZEN", QStringLiteral("账号已被冻结，无法进行排队操作")); return false;
    }
    return true;
}
bool releaseCall(const QSqlDatabase& db, qint64 chargerId, const QString& now, QString* error)
{
    QSqlQuery release(db);
    release.prepare(QStringLiteral(
        "UPDATE chargers SET status='AVAILABLE',updated_at=? WHERE id=? AND status='RESERVED' "
        "AND NOT EXISTS(SELECT 1 FROM reservations r WHERE r.charger_id=chargers.id AND r.status='ACTIVE') "
        "AND NOT EXISTS(SELECT 1 FROM orders o WHERE o.charger_id=chargers.id AND o.status='CHARGING') "
        "AND NOT EXISTS(SELECT 1 FROM queue_entries q WHERE q.charger_id=chargers.id AND q.status='CALLED') "
        "AND NOT EXISTS(SELECT 1 FROM repair_reports rp WHERE rp.charger_id=chargers.id AND rp.status IN ('ACCEPTED','PROCESSING')) "
        "AND NOT EXISTS(SELECT 1 FROM charger_exceptions e WHERE e.charger_id=chargers.id AND e.status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING'))"));
    release.addBindValue(now); release.addBindValue(chargerId);
    if (!release.exec()) { if (error) *error = release.lastError().text(); return false; }
    return true;
}
} // namespace

QueueRepository::QueueRepository(const QSqlDatabase& database) : database_(database) {}

bool QueueRepository::tick(const QDateTime& now, QString* diagnostic) const
{
    QString local;
    QString* error = diagnostic ? diagnostic : &local;
    error->clear();
    if (!now.isValid() || !database_.isOpen()) { *error = "Invalid queue clock/database"; return false; }
    Transaction transaction(database_);
    if (!transaction.begin(error)) return false;
    // A user made ineligible while waiting must not indefinitely block later users.
    QSqlQuery expired(database_);
    expired.prepare(QStringLiteral(
        "SELECT q.id,q.user_id,q.charger_id,q.status FROM queue_entries q "
        "JOIN users u ON u.id=q.user_id JOIN chargers c ON c.id=q.charger_id "
        "JOIN stations s ON s.id=c.station_id WHERE q.status IN ('WAITING','CALLED') AND "
        "(u.status<>'ACTIVE' OR EXISTS(SELECT 1 FROM orders o WHERE o.user_id=q.user_id "
        "AND o.status IN ('RESERVED','CHARGING','WAITING_PAYMENT')) "
        "OR (q.status='CALLED' AND (q.call_expires_at<=? OR c.status<>'RESERVED' OR s.status<>'ACTIVE' "
        "OR EXISTS(SELECT 1 FROM repair_reports rp WHERE rp.charger_id=c.id AND rp.status IN ('ACCEPTED','PROCESSING')) "
        "OR EXISTS(SELECT 1 FROM charger_exceptions e WHERE e.charger_id=c.id AND e.status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING')))))"));
    expired.addBindValue(utc(now));
    if (!expired.exec()) { *error = expired.lastError().text(); return false; }
    struct Expiry { qint64 id; qint64 user; qint64 charger; bool called; };
    QList<Expiry> due;
    while (expired.next()) due.append({expired.value(0).toLongLong(), expired.value(1).toLongLong(),
                                      expired.value(2).toLongLong(), expired.value(3).toString() == "CALLED"});
    expired.finish();
    for (const auto& entry : due) {
        QSqlQuery update(database_);
        update.prepare("UPDATE queue_entries SET status='EXPIRED',ended_at=?,updated_at=? WHERE id=? AND status IN ('WAITING','CALLED')");
        update.addBindValue(utc(now)); update.addBindValue(utc(now)); update.addBindValue(entry.id);
        if (!update.exec() || update.numRowsAffected() != 1) { *error = update.lastError().text(); return false; }
        if (entry.called && !releaseCall(database_, entry.charger, utc(now), error)) return false;
        if (!repository_detail::insertNotificationInTransaction(database_, entry.user, "QUEUE_EXPIRED",
                QStringLiteral("排队机会已失效"),
                QStringLiteral("排队编号 %1 的叫号已超时或当前已不具备排队条件，电桩将继续分配给下一位。您可重新加入队列。")
                    .arg(entry.id), now, error)) return false;
    }

    QSqlQuery available(database_);
    if (!available.exec(QStringLiteral(
            "SELECT c.id FROM chargers c JOIN stations s ON s.id=c.station_id "
            "WHERE c.status='AVAILABLE' AND s.status='ACTIVE' "
            "AND EXISTS(SELECT 1 FROM queue_entries q WHERE q.charger_id=c.id AND q.status='WAITING') "
            "AND NOT EXISTS(SELECT 1 FROM queue_entries q WHERE q.charger_id=c.id AND q.status='CALLED') "
            "AND NOT EXISTS(SELECT 1 FROM reservations r WHERE r.charger_id=c.id AND r.status='ACTIVE') "
            "AND NOT EXISTS(SELECT 1 FROM orders o WHERE o.charger_id=c.id AND o.status='CHARGING') "
            "AND NOT EXISTS(SELECT 1 FROM repair_reports rp WHERE rp.charger_id=c.id AND rp.status IN ('ACCEPTED','PROCESSING')) "
            "AND NOT EXISTS(SELECT 1 FROM charger_exceptions e WHERE e.charger_id=c.id AND e.status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING')) "
            "ORDER BY c.id"))) { *error = available.lastError().text(); return false; }
    QList<qint64> chargers;
    while (available.next()) chargers.append(available.value(0).toLongLong());
    available.finish();
    for (qint64 charger : chargers) {
        QSqlQuery head(database_);
        head.prepare("SELECT id,user_id FROM queue_entries WHERE charger_id=? AND status='WAITING' ORDER BY entered_at,id LIMIT 1");
        head.addBindValue(charger);
        if (!head.exec()) { *error = head.lastError().text(); return false; }
        if (!head.next()) continue;
        const qint64 id = head.value(0).toLongLong(), user = head.value(1).toLongLong(); head.finish();
        QSqlQuery hold(database_);
        hold.prepare("UPDATE chargers SET status='RESERVED',updated_at=? WHERE id=? AND status='AVAILABLE'");
        hold.addBindValue(utc(now)); hold.addBindValue(charger);
        if (!hold.exec() || hold.numRowsAffected() != 1) { *error = hold.lastError().text(); return false; }
        QSqlQuery call(database_);
        call.prepare("UPDATE queue_entries SET status='CALLED',called_at=?,call_expires_at=?,updated_at=? WHERE id=? AND status='WAITING'");
        call.addBindValue(utc(now)); call.addBindValue(utc(now.addSecs(kCallLifetimeSeconds)));
        call.addBindValue(utc(now)); call.addBindValue(id);
        if (!call.exec() || call.numRowsAffected() != 1) { *error = call.lastError().text(); return false; }
        if (!repository_detail::insertNotificationInTransaction(database_, user, "QUEUE_CALLED",
                QStringLiteral("轮到您使用电桩了"),
                QStringLiteral("排队编号 %1 已叫号，请在 60 秒内打开“我的排队”确认；确认后进入 15 分钟预约倒计时。")
                    .arg(id), now, error)) return false;
    }
    return transaction.commit(error);
}

QJsonObject QueueRepository::execute(const QString& type, const QJsonObject& data, qint64 userId,
                                     const QDateTime& now) const
{
    QJsonObject authError;
    if (!hasActiveUser(database_, userId, &authError)) return authError;
    if (!tick(now)) return dbFail();
    if (type == "QUEUE_GET_MINE") {
        QSqlQuery q(database_);
        q.prepare("SELECT " + kColumns + kFrom +
                  " WHERE q.user_id=? ORDER BY CASE WHEN q.status IN ('WAITING','CALLED') THEN 0 ELSE 1 END,q.id DESC LIMIT 1");
        q.addBindValue(userId);
        if (!q.exec()) return dbFail();
        return success({{"item", q.next() ? QJsonValue(row(q, now, false)) : QJsonValue(QJsonValue::Null)},
                        {"serverNow", utc(now)}});
    }
    const QString operation = data.value("operationId").toString();
    const qint64 id = data.value("id").toString().toLongLong();
    const qint64 chargerId = data.value("chargerId").toString().toLongLong();
    if (type == "QUEUE_CONFIRM") {
        QSqlQuery target(database_);
        target.prepare("SELECT charger_id,confirm_operation_id FROM queue_entries WHERE id=? AND user_id=?");
        target.addBindValue(id); target.addBindValue(userId);
        if (!target.exec()) return dbFail();
        if (!target.next()) return fail("NOT_FOUND", QStringLiteral("排队记录不存在或不可访问"));
        const qint64 targetCharger = target.value(0).toLongLong();
        if (!target.value(1).isNull() && target.value(1).toString() != operation)
            return fail("IDEMPOTENCY_CONFLICT", QStringLiteral("请使用原确认操作编号重试"));
        target.finish();
        QSqlQuery duplicate(database_);
        duplicate.prepare("SELECT id FROM queue_entries WHERE user_id=? AND confirm_operation_id=? AND id<>?");
        duplicate.addBindValue(userId); duplicate.addBindValue(operation); duplicate.addBindValue(id);
        if (!duplicate.exec()) return dbFail();
        if (duplicate.next()) return fail("IDEMPOTENCY_CONFLICT", QStringLiteral("操作编号已用于其他排队记录"));
        duplicate.finish();
        ChargingRepository repository(database_);
        const auto result = repository.reserve(userId, targetCharger, now, now.addSecs(15 * 60),
            QStringLiteral("Q") + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'), id, operation);
        if (!result.ok) {
            if (result.error == RepositoryError::Database) return dbFail();
            if (result.error == RepositoryError::ExistingUnfinishedOrder)
                return fail("CONFLICT", QStringLiteral("请先处理当前未完成订单"));
            return fail("INVALID_STATE_TRANSITION", QStringLiteral("叫号已过期或电桩暂不可用，请刷新排队状态"));
        }
        QJsonObject item;
        if (!readItem(database_, id, now, &item)) return dbFail();
        return success({{"item", item}, {"reservation", charging::model::toJson(result.reservation)},
                        {"order", charging::model::toJson(result.order)},
                        {"charger", charging::model::toJson(result.charger)},
                        {"idempotent", result.idempotent}, {"serverNow", utc(now)}});
    }

    Transaction transaction(database_);
    if (!transaction.begin()) return dbFail();
    if (type == "QUEUE_JOIN") {
        QSqlQuery duplicate(database_);
        duplicate.prepare("SELECT id,charger_id FROM queue_entries WHERE user_id=? AND join_operation_id=?");
        duplicate.addBindValue(userId); duplicate.addBindValue(operation);
        if (!duplicate.exec()) return dbFail();
        if (duplicate.next()) {
            const qint64 originalId = duplicate.value(0).toLongLong();
            if (duplicate.value(1).toLongLong() != chargerId)
                return fail("IDEMPOTENCY_CONFLICT", QStringLiteral("操作编号与原排队电桩不一致"));
            duplicate.finish();
            if (!transaction.commit()) return dbFail();
            return itemReply(database_, originalId, now, true);
        }
        QSqlQuery unfinished(database_);
        unfinished.prepare("SELECT 1 FROM orders WHERE user_id=? AND status IN ('RESERVED','CHARGING','WAITING_PAYMENT') UNION ALL SELECT 1 FROM queue_entries WHERE user_id=? AND status IN ('WAITING','CALLED') LIMIT 1");
        unfinished.addBindValue(userId); unfinished.addBindValue(userId);
        if (!unfinished.exec()) return dbFail();
        if (unfinished.next()) return fail("CONFLICT", QStringLiteral("请先处理当前订单或退出已有排队"));
        QSqlQuery station(database_);
        station.prepare(QStringLiteral(
            "SELECT s.status,(SELECT count(*) FROM chargers a WHERE a.station_id=s.id AND a.status='AVAILABLE' "
            "AND NOT EXISTS(SELECT 1 FROM repair_reports rp WHERE rp.charger_id=a.id AND rp.status IN ('ACCEPTED','PROCESSING')) "
            "AND NOT EXISTS(SELECT 1 FROM charger_exceptions e WHERE e.charger_id=a.id AND e.status IN ('ACTIVE','ACKNOWLEDGED','RECOVERING'))),"
            "EXISTS(SELECT 1 FROM repair_reports rp WHERE rp.charger_id=c.id AND rp.status IN ('ACCEPTED','PROCESSING')) "
            "FROM chargers c JOIN stations s ON s.id=c.station_id WHERE c.id=?"));
        station.addBindValue(chargerId);
        if (!station.exec()) return dbFail();
        if (!station.next()) return fail("NOT_FOUND", QStringLiteral("电桩不存在"));
        if (station.value(0).toString() != "ACTIVE" || station.value(2).toBool())
            return fail("CHARGER_NOT_AVAILABLE", QStringLiteral("电站暂停营业或电桩维护中，暂不能加入排队"));
        if (station.value(1).toInt() > 0)
            return fail("CONFLICT", QStringLiteral("本站还有可用电桩，请直接预约"));
        station.finish();
        QSqlQuery insert(database_);
        insert.prepare("INSERT INTO queue_entries(user_id,charger_id,status,entered_at,updated_at,join_operation_id) VALUES (?,?,'WAITING',?,?,?)");
        insert.addBindValue(userId); insert.addBindValue(chargerId); insert.addBindValue(utc(now));
        insert.addBindValue(utc(now)); insert.addBindValue(operation);
        if (!insert.exec()) return dbFail();
        const qint64 createdId = insert.lastInsertId().toLongLong();
        if (!transaction.commit()) return dbFail();
        if (!tick(now)) return dbFail();
        return itemReply(database_, createdId, now, false);
    }
    if (type == "QUEUE_LEAVE") {
        QSqlQuery target(database_);
        target.prepare("SELECT charger_id,status,leave_operation_id FROM queue_entries WHERE id=? AND user_id=?");
        target.addBindValue(id); target.addBindValue(userId);
        if (!target.exec()) return dbFail();
        if (!target.next()) return fail("NOT_FOUND", QStringLiteral("排队记录不存在或不可访问"));
        const qint64 targetCharger = target.value(0).toLongLong();
        const QString status = target.value(1).toString();
        if (status == "LEFT" && target.value(2).toString() == operation) {
            target.finish(); if (!transaction.commit()) return dbFail();
            return itemReply(database_, id, now, true);
        }
        if (status != "WAITING" && status != "CALLED")
            return fail("INVALID_STATE_TRANSITION", QStringLiteral("该排队记录已结束，请刷新"));
        target.finish();
        QSqlQuery duplicate(database_);
        duplicate.prepare("SELECT 1 FROM queue_entries WHERE user_id=? AND leave_operation_id=? AND id<>?");
        duplicate.addBindValue(userId); duplicate.addBindValue(operation); duplicate.addBindValue(id);
        if (!duplicate.exec()) return dbFail();
        if (duplicate.next()) return fail("IDEMPOTENCY_CONFLICT", QStringLiteral("操作编号已用于其他排队记录"));
        duplicate.finish();
        QSqlQuery leave(database_);
        leave.prepare("UPDATE queue_entries SET status='LEFT',ended_at=?,updated_at=?,leave_operation_id=? WHERE id=? AND user_id=? AND status IN ('WAITING','CALLED')");
        leave.addBindValue(utc(now)); leave.addBindValue(utc(now)); leave.addBindValue(operation);
        leave.addBindValue(id); leave.addBindValue(userId);
        if (!leave.exec() || leave.numRowsAffected() != 1) return dbFail();
        if (status == "CALLED" && !releaseCall(database_, targetCharger, utc(now), nullptr)) return dbFail();
        if (!transaction.commit() || !tick(now)) return dbFail();
        return itemReply(database_, id, now, false);
    }
    return fail("UNKNOWN_REQUEST_TYPE", QStringLiteral("未知排队请求"));
}

QJsonObject QueueRepository::adminRead(const QJsonObject& data, const QDateTime& now) const
{
    if (!tick(now)) return dbFail();
    QString condition = " WHERE 1=1";
    QVariantList binds;
    for (const QString& name : {QStringLiteral("stationId"), QStringLiteral("chargerId")}) {
        if (!data.contains(name)) continue;
        condition += name == "stationId" ? " AND c.station_id=?" : " AND q.charger_id=?";
        binds.append(data.value(name).toString().toLongLong());
    }
    const QString status = data.value("status").toString("ACTIVE");
    if (status == "ACTIVE") condition += " AND q.status IN ('WAITING','CALLED')";
    else if (status != "ALL") { condition += " AND q.status=?"; binds.append(status); }
    QSqlQuery count(database_);
    count.prepare("SELECT count(*)" + kFrom + condition);
    for (const auto& value : binds) count.addBindValue(value);
    if (!count.exec() || !count.next()) return dbFail();
    const int total = count.value(0).toInt(); count.finish();
    const int page = data.value("page").toInt(1), pageSize = data.value("pageSize").toInt(20);
    QSqlQuery list(database_);
    list.prepare("SELECT " + kColumns + kFrom + condition + " ORDER BY c.station_id,q.charger_id,q.entered_at,q.id LIMIT ? OFFSET ?");
    for (const auto& value : binds) list.addBindValue(value);
    list.addBindValue(pageSize); list.addBindValue(static_cast<qint64>(page - 1) * pageSize);
    if (!list.exec()) return dbFail();
    QJsonArray items;
    while (list.next()) items.append(row(list, now, true));
    return success({{"items", items}, {"total", total}, {"page", page}, {"pageSize", pageSize}, {"serverNow", utc(now)}});
}
} // namespace charging::server
