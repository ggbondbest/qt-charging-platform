#pragma once

#include "charging/common/model/models.h"
#include "workflow_repository_types.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace charging::server {

struct RechargeResult
{
    bool ok = false;
    bool idempotent = false;
    RepositoryError error = RepositoryError::None;
    charging::model::RechargeRecord record;
    QString diagnostic;
};

struct RechargeQueryResult
{
    bool ok = false;
    QVector<charging::model::RechargeRecord> records;
    int totalCount = 0;
    QString errorMessage;
};

class RechargeRepository final
{
public:
    explicit RechargeRepository(const QSqlDatabase& database);

    RechargeResult recharge(qint64 userId, const QString& transactionNo, qint64 amountCents,
                            const QDateTime& createdAtUtc) const;
    RechargeQueryResult listByUser(qint64 userId, int limit = 20, int offset = 0) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
