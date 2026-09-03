#pragma once

#include "workflow_repository_types.h"

#include <QDateTime>
#include <QSqlDatabase>

namespace charging::server {

class OrderRepository final
{
public:
    explicit OrderRepository(const QSqlDatabase& database);

    OrderRepositoryResult pay(qint64 userId, qint64 orderId, const QDateTime& paidAtUtc) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
