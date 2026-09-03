#include "charging/client/profile_charging/order_service.h"

#include "charging/common/model/enums.h"
#include "charging/common/model/model_json.h"

#include <QJsonArray>
#include <QJsonObject>

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
    // TODO(contract): status/page/pageSize/total names are not frozen in
    // docs/api/socket_protocol.md yet; align with the leader before wiring
    // the real transport. Display fields stationName/chargerCode are read
    // from the same object as the order payload (server-side join expected).
    const QString status = filterToStatus(filter);
    if (!status.isEmpty()) {
        payload.insert(QStringLiteral("status"), status);
    }
    payload.insert(QStringLiteral("page"), safePage);
    payload.insert(QStringLiteral("pageSize"), kOrdersPageSize);

    transport_->send(
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
            const bool hasMore = safePage * kOrdersPageSize < total;
            emit ordersLoaded(orders, total, hasMore);
        });
}

} // namespace charging::client
