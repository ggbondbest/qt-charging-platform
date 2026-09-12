#include "charging/client/profile_charging/order_service.h"

#include "charging/common/model/enums.h"
#include "charging/common/model/model_json.h"

#include <QJsonArray>
#include <QJsonObject>
#include "network/page_validation.h"

namespace charging::client {

namespace {

charging::protocol::ProtocolError makeLocalError(const char* code, const QString& message)
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(code);
    error.message = message;
    return error;
}

} // namespace

OrderService::OrderService(IRequestTransport* transport, QObject* parent)
    : QObject(parent), transport_(transport)
{
}

bool OrderService::isFetchingOrders() const
{
    return fetchingOrders_;
}

bool OrderService::isFetchingStatusCounts() const
{
    return countingOrders_;
}

void OrderService::fetchStatusCounts()
{
    if (countingOrders_ || transport_ == nullptr) {
        return; // In-flight guard; a missing transport degrades silently (see header).
    }

    countingOrders_ = true;
    countRequestsFailed_ = false;
    pendingCountRequests_ = 0;

    // One page-1/pageSize-1 request per badge status; we only need `total`.
    // status/page/pageSize/total are frozen in docs/api/user_api_contract.md.
    // Production HomeShell injects NetworkRequestTransport; previews use Mock.
    const Filter filters[] = {
        Filter::Charging,
        Filter::WaitingPayment,
        Filter::Completed,
    };

    for (const Filter filter : filters) {
        ++pendingCountRequests_;
        QJsonObject payload;
        payload.insert(QStringLiteral("status"), filterToStatus(filter));
        payload.insert(QStringLiteral("page"), 1);
        payload.insert(QStringLiteral("pageSize"), 1);

        transport_->sendFor(this,
            QString::fromLatin1(charging::protocol::request_type::kGetOrders), payload,
            [this, filter](bool success, const QJsonObject& data,
                           const charging::protocol::ProtocolError& /*error*/) {
                bool more = false;
                if (success && network::readPage(data, "orders", 1, 1, &more)) {
                    const int total = data.value(QStringLiteral("total")).toInt();
                    switch (filter) {
                    case Filter::Charging:
                        chargingCount_ = total;
                        break;
                    case Filter::WaitingPayment:
                        waitingPaymentCount_ = total;
                        break;
                    case Filter::Completed:
                        completedCount_ = total;
                        break;
                    case Filter::All:
                        break;
                    }
                } else {
                    countRequestsFailed_ = true;
                }
                if (--pendingCountRequests_ > 0) {
                    return; // Wait for the remaining badge queries.
                }
                countingOrders_ = false;
                if (!countRequestsFailed_) {
                    emit statusCountsUpdated(chargingCount_, waitingPaymentCount_,
                                             completedCount_);
                }
                // On failure keep the previous badges: the counts are a
                // decoration for the profile hub and must not toast.
            });
    }
}

QString OrderService::filterToStatus(Filter filter)
{
    switch (filter) {
    case Filter::Charging:
        return charging::model::toString(charging::model::OrderStatus::Charging);
    case Filter::WaitingPayment:
        return charging::model::toString(charging::model::OrderStatus::WaitingPayment);
    case Filter::Completed:
        return charging::model::toString(charging::model::OrderStatus::Completed);
    case Filter::All:
        break;
    }
    return QString();
}

void OrderService::fetchOrders(Filter filter, int page)
{
    const QString type = QString::fromLatin1(charging::protocol::request_type::kGetOrders);
    if (fetchingOrders_) {
        return; // Ignore duplicate submissions while one list request is in flight.
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }
    const int safePage = page > 0 ? page : 1;

    fetchingOrders_ = true;
    QJsonObject payload;
    // Frozen contract: pagination plus flat Order + stationName/chargerCode.
    // Server joins are user-scoped; pages do not query SQL.
    const QString status = filterToStatus(filter);
    if (!status.isEmpty()) {
        payload.insert(QStringLiteral("status"), status);
    }
    payload.insert(QStringLiteral("page"), safePage);
    payload.insert(QStringLiteral("pageSize"), kOrdersPageSize);

    transport_->sendFor(this,
        type, payload,
        [this, safePage](bool success, const QJsonObject& data,
                         const charging::protocol::ProtocolError& error) {
            fetchingOrders_ = false;
            const QString ordersType =
                QString::fromLatin1(charging::protocol::request_type::kGetOrders);
            if (!success) {
                emit operationFailed(ordersType, error);
                return;
            }

            bool hasMore = false;
            if (!network::readPage(data, "orders", safePage, kOrdersPageSize, &hasMore)) {
                emit operationFailed(ordersType, makeLocalError(charging::protocol::error_code::kInvalidEnvelope,
                                                               QStringLiteral("订单分页响应无效")));
                return;
            }
            QVector<charging::client::OrderSummary> orders;
            const QJsonArray array = data.value(QStringLiteral("orders")).toArray();
            for (const QJsonValue& value : array) {
                const QJsonObject object = value.toObject();
                charging::client::OrderSummary summary;
                QString parseError;
                if (!charging::model::fromJson(object, &summary.order, &parseError)) {
                    emit operationFailed(
                        ordersType,
                        makeLocalError(charging::protocol::error_code::kInternalError,
                                       QStringLiteral("invalid order payload: ") + parseError));
                    return;
                }
                summary.stationName = object.value(QStringLiteral("stationName")).toString();
                summary.chargerCode = object.value(QStringLiteral("chargerCode")).toString();
                orders.append(summary);
            }

            const int total = data.value(QStringLiteral("total")).toInt();
            emit ordersLoaded(orders, total, hasMore);
        });
}

} // namespace charging::client
