#pragma once

#include "workflow_repository_types.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <optional>

namespace charging::server {

struct OrderListItem
{
    charging::model::Order order;
    QString userPhone;
    QString userNickname;
    QString stationName;
    QString chargerCode;
};

struct OrderQuery
{
    QString keyword;
    std::optional<charging::model::OrderStatus> status;
    int limit = 20;
    int offset = 0;
};

struct OrderQueryResult
{
    bool ok = false;
    QVector<OrderListItem> orders;
    int totalCount = 0;
    QString errorMessage;
};

class OrderRepository final
{
public:
    explicit OrderRepository(const QSqlDatabase& database);

    OrderQueryResult list(const OrderQuery& query) const;
    OrderRepositoryResult pay(qint64 userId, qint64 orderId, const QDateTime& paidAtUtc) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
