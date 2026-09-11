#include "operation_log_repository.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace charging::server {
namespace {

constexpr int kMaximumPageSize = 100;

bool readLog(const QSqlQuery& query, charging::model::OperationLog* log)
{
    bool idOk = false;
    bool adminIdOk = true;
    charging::model::OperationLog value;
    value.id = query.value(0).toLongLong(&idOk);
    if (!query.value(1).isNull()) {
        value.adminId = query.value(1).toLongLong(&adminIdOk);
    }
    value.action = query.value(2).toString();
    value.targetType = query.value(3).toString();
    value.targetId = query.value(4).toString();
    QJsonParseError parseError;
    const QJsonDocument details =
        QJsonDocument::fromJson(query.value(5).toString().toUtf8(), &parseError);
    value.createdAtUtc = QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs);
    if (!idOk || !adminIdOk || value.id <= 0 || value.action.isEmpty() ||
        value.targetType.isEmpty() || parseError.error != QJsonParseError::NoError ||
        !details.isObject() || !value.createdAtUtc.isValid()) {
        return false;
    }
    value.details = details.object();
    value.createdAtUtc = value.createdAtUtc.toUTC();
    *log = value;
    return true;
}

} // namespace

OperationLogRepository::OperationLogRepository(const QSqlDatabase& database)
    : database_(database)
{
}

OperationLogResult OperationLogRepository::append(qint64 adminId, const QString& action,
                                                  const QString& targetType,
                                                  const QString& targetId,
                                                  const QJsonObject& details,
                                                  const QDateTime& createdAtUtc) const
{
    OperationLogResult result;
    if (adminId < 0 || action.trimmed().isEmpty() || action.trimmed().size() > 64 ||
        targetType.trimmed().isEmpty() || targetType.trimmed().size() > 32 ||
        !createdAtUtc.isValid()) {
        result.errorMessage = QStringLiteral("Invalid operation log parameters");
        return result;
    }
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO operation_logs "
        "(admin_id, action, target_type, target_id, details_json, created_at) "
        "VALUES (:adminId, :action, :targetType, :targetId, :details, :createdAt)"));
    if (adminId == 0) {
        query.bindValue(QStringLiteral(":adminId"), QVariant());
    } else {
        query.bindValue(QStringLiteral(":adminId"), adminId);
    }
    query.bindValue(QStringLiteral(":action"), action.trimmed());
    query.bindValue(QStringLiteral(":targetType"), targetType.trimmed());
    query.bindValue(QStringLiteral(":targetId"), targetId);
    query.bindValue(QStringLiteral(":details"),
                    QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact)));
    query.bindValue(QStringLiteral(":createdAt"),
                    createdAtUtc.toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        result.errorMessage = query.lastError().text();
        return result;
    }

    QSqlQuery reload(database_);
    reload.prepare(QStringLiteral(
        "SELECT id, admin_id, action, target_type, target_id, details_json, created_at "
        "FROM operation_logs WHERE id = :id"));
    reload.bindValue(QStringLiteral(":id"), query.lastInsertId());
    if (!reload.exec() || !reload.next() || !readLog(reload, &result.log)) {
        result.errorMessage = reload.lastError().text().isEmpty()
            ? QStringLiteral("The inserted operation log could not be reloaded")
            : reload.lastError().text();
        return result;
    }
    result.ok = true;
    return result;
}

OperationLogQueryResult OperationLogRepository::list(qint64 adminId, const QString& action,
                                                      int limit, int offset) const
{
    OperationLogQueryResult result;
    if (!database_.isValid() || !database_.isOpen()) {
        result.errorMessage = QStringLiteral("The SQLite connection is not open");
        return result;
    }
    if (adminId < 0 || limit <= 0 || limit > kMaximumPageSize || offset < 0) {
        result.errorMessage = QStringLiteral("Invalid operation log query parameters");
        return result;
    }

    QString filters = QStringLiteral(" WHERE action LIKE :action COLLATE NOCASE");
    if (adminId > 0) {
        filters.append(QStringLiteral(" AND admin_id = :adminId"));
    }
    const auto bindFilters = [adminId, action](QSqlQuery* query) {
        query->bindValue(QStringLiteral(":action"),
                         QStringLiteral("%%1%").arg(action.trimmed()));
        if (adminId > 0) {
            query->bindValue(QStringLiteral(":adminId"), adminId);
        }
    };

    QSqlQuery countQuery(database_);
    countQuery.prepare(QStringLiteral("SELECT COUNT(*) FROM operation_logs") + filters);
    bindFilters(&countQuery);
    if (!countQuery.exec() || !countQuery.next()) {
        result.errorMessage = countQuery.lastError().text();
        return result;
    }
    result.totalCount = countQuery.value(0).toInt();

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
                      "SELECT id, admin_id, action, target_type, target_id, details_json, "
                      "created_at FROM operation_logs")
                  + filters
                  + QStringLiteral(" ORDER BY created_at DESC, id DESC LIMIT :limit OFFSET :offset"));
    bindFilters(&query);
    query.bindValue(QStringLiteral(":limit"), limit);
    query.bindValue(QStringLiteral(":offset"), offset);
    if (!query.exec()) {
        result.errorMessage = query.lastError().text();
        return result;
    }
    while (query.next()) {
        charging::model::OperationLog log;
        if (!readLog(query, &log)) {
            result.errorMessage = QStringLiteral("The stored operation log row contains invalid data");
            result.logs.clear();
            return result;
        }
        result.logs.append(log);
    }
    result.ok = true;
    return result;
}

} // namespace charging::server
