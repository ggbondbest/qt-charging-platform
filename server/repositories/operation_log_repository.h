#pragma once

#include "charging/common/model/models.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace charging::server {

struct OperationLogResult
{
    bool ok = false;
    charging::model::OperationLog log;
    QString errorMessage;
};

struct OperationLogQueryResult
{
    bool ok = false;
    QVector<charging::model::OperationLog> logs;
    int totalCount = 0;
    QString errorMessage;
};

class OperationLogRepository final
{
public:
    explicit OperationLogRepository(const QSqlDatabase& database);

    OperationLogResult append(qint64 adminId, const QString& action, const QString& targetType,
                              const QString& targetId, const QJsonObject& details,
                              const QDateTime& createdAtUtc) const;
    OperationLogQueryResult list(qint64 adminId = 0, const QString& action = {}, int limit = 20,
                                 int offset = 0) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
