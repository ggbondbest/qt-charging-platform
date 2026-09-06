#pragma once

// Same-name forwarding bridges for CONTRACT.md §1.
//
// Landmine found while wiring pages: the service classes expose their API as
// plain C++ member functions (not slots), and their signals carry plain
// structs (Order/User/QVector<RechargeRecord>) that QML cannot introspect.
// So the context properties injected as `walletService`/`orderService`/
// `chargingService` are NOT the raw C++ objects — they are these bridges,
// which keep the contract names *verbatim*: same method names (Q_INVOKABLE),
// same signal names, only the payload types become QML-readable variants.
// Enum arguments (Filter, OrderStatus…) cross as lowercase strings.
//
// Raw services stay untouched (old widgets keep using them directly).

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "charging/common/model/enums.h"

namespace charging::client {
class WalletService;
class OrderService;
class ChargingService;
struct OrderSummary;
} // namespace charging::client

namespace charging::model {
struct Order;
struct RechargeRecord;
struct User;
} // namespace charging::model

namespace charging::qml {

namespace marshalling {
QVariantMap userToMap(const charging::model::User& user);
QVariantMap orderToMap(const charging::model::Order& order,
                       const QString& stationName = QString(),
                       const QString& chargerCode = QString());
QString orderStatusWord(charging::model::OrderStatus status);
} // namespace marshalling

class WalletBridge final : public QObject
{
    Q_OBJECT
public:
    explicit WalletBridge(charging::client::WalletService* svc, QObject* parent = nullptr);

    Q_INVOKABLE void fetchProfile();
    Q_INVOKABLE void updateNickname(const QString& nickname);
    Q_INVOKABLE void updateAvatar(const QString& avatarKey);
    Q_INVOKABLE void recharge(qint64 amountCents);
    Q_INVOKABLE void fetchRechargeRecords(int page);

signals:
    void profileLoaded(const QVariantMap& user);
    void rechargeCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void rechargeRecordsLoaded(const QVariantList& records, bool hasMore);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::WalletService* svc_;
};

class OrderBridge final : public QObject
{
    Q_OBJECT
public:
    explicit OrderBridge(charging::client::OrderService* svc, QObject* parent = nullptr);

    // filter ∈ "all" | "charging" | "waiting_payment" | "completed"
    Q_INVOKABLE void fetchOrders(const QString& filter, int page);
    Q_INVOKABLE void fetchStatusCounts();
    // In-flight guard mirror: fetchOrders silently drops while one is running,
    // so pages must check before paginating or they'd freeze the refresh pill.
    Q_INVOKABLE bool isFetchingOrders() const;

signals:
    void ordersLoaded(const QVariantList& orders, int total, bool hasMore);
    void statusCountsUpdated(int chargingCount, int waitingPaymentCount, int completedCount);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::OrderService* svc_;
};

class ChargingBridge final : public QObject
{
    Q_OBJECT
public:
    explicit ChargingBridge(charging::client::ChargingService* svc, QObject* parent = nullptr);

    Q_INVOKABLE void startTracking(qint64 orderId);
    Q_INVOKABLE void stopTracking();
    Q_INVOKABLE void fetchStatusNow();
    Q_INVOKABLE void stopCharging();
    Q_INVOKABLE void startCharging(qint64 reservationId);
    Q_INVOKABLE void payOrder(qint64 orderId);

signals:
    void statusLoaded(const QVariantMap& status);
    void startCompleted(const QVariantMap& status);
    void stopCompleted(const QVariantMap& status);
    void paymentCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::ChargingService* svc_;
};

} // namespace charging::qml
