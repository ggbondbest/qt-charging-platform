#pragma once

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/model/models.h"

#include <QObject>
#include <QVector>

namespace charging::client {

// A persisted order plus the station/charger display strings the detail and
// list pages need. The Order model only carries chargerId; the joined display
// fields are expected from the server response (see TODO(contract) below),
// never by adding columns to the database.
struct OrderSummary
{
    charging::model::Order order;
    QString stationName;
    QString chargerCode;
};

// User-facing use cases for the order history list.
// Wire action: GET_ORDERS (candidate-v1 registry).
class OrderService final : public QObject
{
    Q_OBJECT

public:
    static constexpr int kOrdersPageSize = 10;

    enum class Filter
    {
        All,
        Charging,
        WaitingPayment,
        Completed
    };

    explicit OrderService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isFetchingOrders() const;
    bool isFetchingStatusCounts() const;

    void fetchOrders(Filter filter, int page); // page from 1

    // Badge counts for the profile hub. Issues one lightweight GET_ORDERS per
    // status (page=1, pageSize=1) and reads only the `total` field — the
    // protocol has no dedicated count action and we must not invent one.
    // Failures degrade silently (badges keep their previous state and no
    // operationFailed is emitted, so an incidental refresh can never toast
    // from an unrelated page).
    void fetchStatusCounts();

signals:
    void ordersLoaded(const QVector<charging::client::OrderSummary>& orders, int total,
                      bool hasMore);
    void statusCountsUpdated(int chargingCount, int waitingPaymentCount, int completedCount);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    static QString filterToStatus(Filter filter);

    IRequestTransport* transport_ = nullptr;
    bool fetchingOrders_ = false;
    bool countingOrders_ = false;
    int pendingCountRequests_ = 0;
    bool countRequestsFailed_ = false;
    int chargingCount_ = 0;
    int waitingPaymentCount_ = 0;
    int completedCount_ = 0;
};

} // namespace charging::client

// Enables OrderSummary in queued signal/slot connections and QSignalSpy.
Q_DECLARE_METATYPE(charging::client::OrderSummary)
