#include "repair_repository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariantList>

namespace charging::server {
namespace {
QSqlQuery sql(const QSqlDatabase& db, const QString& statement, const QVariantList& values = {})
{
    QSqlQuery q(db);
    if (!q.prepare(statement)) throw RepairFailure("DATABASE_ERROR");
    for (const auto& value : values) q.addBindValue(value);
    if (!q.exec()) throw RepairFailure("DATABASE_ERROR");
    return q;
}
qint64 count(const QSqlDatabase& db, const QString& statement, const QVariantList& values = {})
{
    auto q = sql(db, statement, values);
    if (!q.next()) throw RepairFailure("DATABASE_ERROR");
    return q.value(0).toLongLong();
}
class Transaction final
{
public:
    explicit Transaction(const QSqlDatabase& db) : db_(db) { sql(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) db_.rollback(); }
    void commit() { sql(db_, "COMMIT"); committed_ = true; }
private:
    QSqlDatabase db_;
    bool committed_ = false;
};
void userActive(const QSqlDatabase& db, qint64 userId)
{
    auto q = sql(db, "SELECT status FROM users WHERE id=?", {userId});
    if (!q.next()) throw RepairFailure("UNAUTHORIZED");
    if (q.value(0).toString() != "ACTIVE") throw RepairFailure("USER_FROZEN");
}
void adminActive(const QSqlDatabase& db, qint64 adminId)
{
    if (!count(db, "SELECT COUNT(*) FROM admins WHERE id=? AND status='ACTIVE'", {adminId}))
        throw RepairFailure("UNAUTHORIZED");
}
QString columns()
{
    return QStringLiteral("r.id,r.user_id,r.charger_id,r.problem_type,r.description,r.status,"
                          "r.processing_note,r.created_at,r.updated_at,r.resolved_at,"
                          "c.code,c.station_id,s.name,u.nickname,u.phone,"
                          "EXISTS(SELECT 1 FROM repair_reports m WHERE m.charger_id=r.charger_id "
                          "AND m.status IN ('ACCEPTED','PROCESSING')) AS maintenance");
}
QString joins()
{
    return QStringLiteral(" FROM repair_reports r JOIN chargers c ON c.id=r.charger_id "
                          "JOIN stations s ON s.id=c.station_id JOIN users u ON u.id=r.user_id ");
}
QJsonObject dto(const QSqlQuery& q, bool admin)
{
    QJsonObject item{{"id", q.value(0).toString()}, {"chargerId", q.value(2).toString()},
        {"problemType", q.value(3).toString()}, {"description", q.value(4).toString()},
        {"status", q.value(5).toString()}, {"processingNote", q.value(6).toString()},
        {"createdAt", q.value(7).toString()}, {"updatedAt", q.value(8).toString()},
        {"resolvedAt", q.value(9).isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(q.value(9).toString())},
        {"chargerCode", q.value(10).toString()}, {"stationId", q.value(11).toString()},
        {"stationName", q.value(12).toString()}, {"maintenance", q.value(15).toBool()}, {"simulated", true}};
    if (admin) {
        const auto phone = q.value(14).toString();
        item.insert("user", QJsonObject{{"id", q.value(1).toString()}, {"nickname", q.value(13).toString()},
             {"phone", phone.size() == 11 ? phone.left(3) + "****" + phone.right(4) : QStringLiteral("—")}});
    }
    return item;
}
QJsonObject report(const QSqlDatabase& db, const QString& id, qint64 userId = 0)
{
    auto q = sql(db, "SELECT " + columns() + joins() + "WHERE r.id=?" +
                        (userId ? " AND r.user_id=?" : ""),
                 userId ? QVariantList{id, userId} : QVariantList{id});
    if (!q.next()) throw RepairFailure("NOT_FOUND");
    auto result = dto(q, !userId);
    q.finish();
    QJsonArray timeline;
    auto events = sql(db, "SELECT id,status,note,created_at FROM repair_timeline WHERE report_id=? ORDER BY id", {id});
    while (events.next()) {
        timeline.append(QJsonObject{{"id", events.value(0).toString()}, {"status", events.value(1).toString()},
                         {"note", events.value(2).toString()}, {"createdAt", events.value(3).toString()}});
    }
    result.insert("timeline", timeline);
    return result;
}
QJsonObject listing(const QSqlDatabase& db, const QJsonObject& p, qint64 userId)
{
    QString where = QStringLiteral(" WHERE 1=1");
    QVariantList values;
    if (userId) { where += " AND r.user_id=?"; values << userId; }
    for (const auto& pair : {qMakePair(QStringLiteral("status"), QStringLiteral("r.status")),
                             qMakePair(QStringLiteral("stationId"), QStringLiteral("c.station_id")),
                             qMakePair(QStringLiteral("chargerId"), QStringLiteral("r.charger_id"))}) {
        if (!p.contains(pair.first)) continue;
        where += " AND " + pair.second + "=?";
        values << p.value(pair.first).toString();
    }
    if (p.contains("keyword")) {
        where += " AND (instr(c.code,?)>0 OR instr(s.name,?)>0)";
        values << p.value("keyword").toString().trimmed() << p.value("keyword").toString().trimmed();
    }
    const int page = p.value("page").toInt(1);
    const int pageSize = p.value("pageSize").toInt(20);
    const qint64 total = count(db, "SELECT COUNT(*)" + joins() + where, values);
    values << pageSize << qint64(page - 1) * pageSize;
    auto q = sql(db, "SELECT " + columns() + joins() + where + " ORDER BY r.id DESC LIMIT ? OFFSET ?", values);
    QJsonArray items;
    while (q.next()) items.append(dto(q, !userId));
    return {{"items", items}, {"total", total}, {"page", page}, {"pageSize", pageSize}};
}
QString payload(const QJsonObject& p) { return QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact)); }
QJsonObject replay(const QSqlDatabase& db, const QString& actor, qint64 actorId,
                   const QString& action, const QJsonObject& p)
{
    auto q = sql(db, "SELECT action,payload_json,result_json FROM repair_operations "
                     "WHERE actor_type=? AND actor_id=? AND operation_id=?",
                 {actor, actorId, p.value("operationId").toString()});
    if (!q.next()) return {};
    if (q.value(0).toString() != action || q.value(1).toString() != payload(p)) throw RepairFailure("CONFLICT");
    auto result = QJsonDocument::fromJson(q.value(2).toByteArray()).object();
    if (result.isEmpty()) throw RepairFailure("DATABASE_ERROR");
    result.insert("idempotent", true);
    return result;
}
void saveOperation(const QSqlDatabase& db, const QString& actor, qint64 actorId, const QString& action,
                   const QJsonObject& p, const QJsonObject& result, const QString& now)
{
    sql(db, "INSERT INTO repair_operations(actor_type,actor_id,operation_id,action,payload_json,result_json,created_at) "
            "VALUES(?,?,?,?,?,?,?)", {actor, actorId, p.value("operationId").toString(), action, payload(p), payload(result), now});
    // No user descriptions, phone numbers or credential material are copied to
    // the public operation log. A failing audit insert rolls back all changes.
    const auto item = result.value("item").toObject();
    const QJsonObject details{{"status", item.value("status")}, {"chargerId", item.value("chargerId")},
                             {"simulated", true}};
    sql(db, "INSERT INTO operation_logs(admin_id,action,target_type,target_id,details_json,created_at) "
            "VALUES(?,?,'REPAIR_REPORT',?,?,?)",
        {actor == "ADMIN" ? QVariant(actorId) : QVariant(), action,
         item.value("id").toString(), payload(details), now});
}
void timeline(const QSqlDatabase& db, const QString& id, const QString& status,
              const QString& note, qint64 adminId, const QString& now)
{
    sql(db, "INSERT INTO repair_timeline(report_id,status,note,admin_id,created_at) VALUES(?,?,?,?,?)",
        {id, status, note, adminId ? QVariant(adminId) : QVariant(), now});
}
bool busy(const QSqlDatabase& db, const QString& chargerId)
{
    return count(db, "SELECT COUNT(*) FROM chargers WHERE id=? AND status IN ('RESERVED','CHARGING')", {chargerId}) ||
           count(db, "SELECT COUNT(*) FROM reservations WHERE charger_id=? AND status='ACTIVE'", {chargerId}) ||
           count(db, "SELECT COUNT(*) FROM orders WHERE charger_id=? AND status IN ('RESERVED','CHARGING')", {chargerId});
}
QString versionAfter(const QString& previous, const QDateTime& now)
{
    const auto old = QDateTime::fromString(previous, Qt::ISODateWithMs);
    if (!old.isValid() || !now.isValid()) throw RepairFailure("DATABASE_ERROR");
    return (now > old ? now : old.addMSecs(1)).toUTC().toString(Qt::ISODateWithMs);
}
} // namespace

RepairRepository::RepairRepository(const QSqlDatabase& database) : database_(database) {}

QJsonObject RepairRepository::handle(const QString& type, const QJsonObject& p, qint64 userId,
                                      const QDateTime& current) const
{
    userActive(database_, userId);
    if (type == "REPAIR_GET_MINE") return listing(database_, p, userId);
    if (type == "REPAIR_GET") return {{"item", report(database_, p.value("id").toString(), userId)}};
    if (type != "REPAIR_SUBMIT") throw RepairFailure("UNKNOWN_REQUEST_TYPE");
    Transaction tx(database_);
    userActive(database_, userId);
    const auto old = replay(database_, "USER", userId, type, p);
    if (!old.isEmpty()) { tx.commit(); return old; }
    const auto chargerId = p.value("chargerId").toString();
    if (!count(database_, "SELECT COUNT(*) FROM chargers WHERE id=?", {chargerId})) throw RepairFailure("NOT_FOUND");
    if (count(database_, "SELECT COUNT(*) FROM repair_reports WHERE user_id=? AND charger_id=? AND status<>'RESOLVED'",
              {userId, chargerId})) throw RepairFailure("ALREADY_EXISTS");
    const auto now = current.toUTC().toString(Qt::ISODateWithMs);
    auto inserted = sql(database_, "INSERT INTO repair_reports(user_id,charger_id,problem_type,description,status,"
                        "processing_note,created_at,updated_at) VALUES(?,?,?,?,'SUBMITTED','',?,?)",
                 {userId, chargerId, p.value("problemType").toString(), p.value("description").toString().trimmed(), now, now});
    const auto reportId = inserted.lastInsertId().toString();
    timeline(database_, reportId, "SUBMITTED", QStringLiteral("报障已提交，等待管理员核实；电桩不会因此直接停用"), 0, now);
    QJsonObject result{{"item", report(database_, reportId, userId)}, {"idempotent", false}};
    saveOperation(database_, "USER", userId, type, p, result, now);
    tx.commit();
    return result;
}

QJsonObject RepairRepository::adminHandle(const QString& action, const QJsonObject& p, qint64 adminId,
                                           const QDateTime& current) const
{
    adminActive(database_, adminId);
    if (action == "repair_reports.list") return listing(database_, p, 0);
    if (action == "repair_reports.get") return {{"item", report(database_, p.value("id").toString())}};
    Transaction tx(database_);
    adminActive(database_, adminId);
    const auto previous = replay(database_, "ADMIN", adminId, action, p);
    if (!previous.isEmpty()) { tx.commit(); return previous; }
    const auto reportId = p.value("id").toString();
    const auto old = report(database_, reportId);
    if (old.value("updatedAt") != p.value("expectedUpdatedAt")) throw RepairFailure("CONFLICT");
    const auto oldStatus = old.value("status").toString();
    QString status;
    if (action == "repair_reports.accept" && oldStatus == "SUBMITTED") status = "ACCEPTED";
    else if (action == "repair_reports.start" && oldStatus == "ACCEPTED") status = "PROCESSING";
    else if (action == "repair_reports.resolve" && oldStatus == "PROCESSING") status = "RESOLVED";
    else throw RepairFailure("INVALID_STATE_TRANSITION");
    const auto chargerId = old.value("chargerId").toString();
    if (busy(database_, chargerId)) throw RepairFailure("RESOURCE_BUSY");
    const auto now = current.toUTC().toString(Qt::ISODateWithMs);
    const auto version = versionAfter(old.value("updatedAt").toString(), current);
    auto chargerVersionQuery = sql(database_, "SELECT updated_at FROM chargers WHERE id=?", {chargerId});
    if (!chargerVersionQuery.next()) throw RepairFailure("NOT_FOUND");
    const auto chargerVersion = versionAfter(chargerVersionQuery.value(0).toString(), current);
    chargerVersionQuery.finish();
    const auto note = p.value("note").toString().trimmed();
    sql(database_, "UPDATE repair_reports SET status=?,processing_note=?,updated_at=?,resolved_at=? WHERE id=?",
        {status, note, version, status == "RESOLVED" ? QVariant(now) : QVariant(), reportId});
    if (status != "RESOLVED") {
        sql(database_, "UPDATE chargers SET status='OFFLINE',updated_at=? WHERE id=?", {chargerVersion, chargerId});
    } else {
        const bool otherRepair = count(database_, "SELECT COUNT(*) FROM repair_reports WHERE charger_id=? "
                                                   "AND status IN ('ACCEPTED','PROCESSING')", {chargerId});
        const bool otherException = count(database_, "SELECT COUNT(*) FROM charger_exceptions WHERE charger_id=? "
                                                      "AND status<>'RECOVERED'", {chargerId});
        const bool stationActive = count(database_, "SELECT COUNT(*) FROM chargers c JOIN stations s ON s.id=c.station_id "
                                                      "WHERE c.id=? AND s.status='ACTIVE'", {chargerId});
        // A repaired issue does not override another repair hold, independent
        // fault, station shutdown, reservation or live charging session.
        if (!otherRepair && !otherException && stationActive)
            sql(database_, "UPDATE chargers SET status='AVAILABLE',updated_at=? WHERE id=?", {chargerVersion, chargerId});
    }
    timeline(database_, reportId, status, note, adminId, now);
    auto item = report(database_, reportId);
    const bool available = count(database_, "SELECT COUNT(*) FROM chargers WHERE id=? AND status='AVAILABLE'", {chargerId});
    const QString statusLabel = status == "ACCEPTED" ? QStringLiteral("已受理，电桩进入维护") :
                                status == "PROCESSING" ? QStringLiteral("处理中（模拟维修）") : QStringLiteral("已恢复（模拟维修完成）");
    QString body = QStringLiteral("%1：%2。处理说明：%3").arg(old.value("chargerCode").toString(), statusLabel, note);
    if (status == "RESOLVED" && !available) body += QStringLiteral("。电桩尚有其他限制，暂不可预约");
    sql(database_, "INSERT INTO notifications(user_id,type,title,body,created_at) VALUES(?,'REPAIR_UPDATED',?,?,?)",
        {old.value("user").toObject().value("id").toString(), QStringLiteral("报障处理进度更新"), body, now});
    QJsonObject result{{"item", item}, {"idempotent", false}, {"chargerAvailable", available}, {"simulated", true}};
    saveOperation(database_, "ADMIN", adminId, action, p, result, now);
    tx.commit();
    return result;
}
} // namespace charging::server
