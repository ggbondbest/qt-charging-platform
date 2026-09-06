#include "service_bridges.h"

#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/order_service.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"

#include <QDateTime>

namespace charging::qml {
namespace {

constexpr const char* kIsoDate = "yyyy-MM-dd hh:mm";

QString when(const QDateTime& dt)
{
    return dt.isValid() ? dt.toString(QLatin1String(kIsoDate)) : QString();
}

QVariantList recordList(const QVector<charging::model::RechargeRecord>& records)
{
    QVariantList out;
    out.reserve(records.size());
    for (const auto& r : records) {
        out.push_back(QVariantMap{
            {QStringLiteral("id"), r.id},
            {QStringLiteral("transactionNo"), r.transactionNo},
            {QStringLiteral("amountCents"), r.amountCents},
            {QStringLiteral("balanceAfterCents"), r.balanceAfterCents},
            {QStringLiteral("createdAt"), when(r.createdAtUtc)},
        });
    }
    return out;
}

QVariantMap statusToMap(const charging::client::ChargingStatus& status)
{
    QVariantMap m = marshalling::orderToMap(status.order);
    m.insert(QStringLiteral("stationName"), status.stationName);
    m.insert(QStringLiteral("chargerCode"), status.chargerCode);
    m.insert(QStringLiteral("powerWatts"), status.powerWatts);
    m.insert(QStringLiteral("powerKnown"), status.powerKnown);
    return m;
}

} // namespace

namespace marshalling {

QVariantMap userToMap(const charging::model::User& user)
{
    return QVariantMap{
        {QStringLiteral("id"), user.id},
        {QStringLiteral("phone"), user.phone},
        {QStringLiteral("nickname"), user.nickname},
        {QStringLiteral("avatarKey"), user.avatarKey},
        {QStringLiteral("balanceCents"), user.balanceCents},
    };
}

QString orderStatusWord(charging::model::OrderStatus status)
{
    switch (status) {
    case charging::model::OrderStatus::Reserved: return QStringLiteral("reserved");
    case charging::model::OrderStatus::Charging: return QStringLiteral("charging");
    case charging::model::OrderStatus::WaitingPayment: return QStringLiteral("waiting_payment");
    case charging::model::OrderStatus::Completed: return QStringLiteral("completed");
    case charging::model::OrderStatus::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("reserved");
}

QVariantMap orderToMap(const charging::model::Order& order,
                       const QString& stationName,
                       const QString& chargerCode)
{
    return QVariantMap{
        {QStringLiteral("id"), order.id},
        {QStringLiteral("orderNo"), order.orderNo},
        {QStringLiteral("status"), orderStatusWord(order.status)},
        {QStringLiteral("unitPriceCentsPerKwh"), order.unitPriceCentsPerKwh},
        {QStringLiteral("energyWh"), order.energyWh},
        {QStringLiteral("durationSeconds"), order.durationSeconds},
        {QStringLiteral("amountCents"), order.amountCents},
        {QStringLiteral("createdAt"), when(order.createdAtUtc)},
        {QStringLiteral("startedAt"), when(order.startedAtUtc)},
        {QStringLiteral("stoppedAt"), when(order.stoppedAtUtc)},
        {QStringLiteral("stationName"), stationName},
        {QStringLiteral("chargerCode"), chargerCode},
    };
}

} // namespace marshalling

// ————————————————————————————— WalletBridge —————————————————————————————

WalletBridge::WalletBridge(charging::client::WalletService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::WalletService::profileLoaded, this,
            [this](const charging::model::User& user) {
                emit profileLoaded(marshalling::userToMap(user));
            });
    connect(svc_, &charging::client::WalletService::rechargeCompleted, this,
            &WalletBridge::rechargeCompleted);
    connect(svc_, &charging::client::WalletService::rechargeRecordsLoaded, this,
            [this](const QVector<charging::model::RechargeRecord>& records, bool hasMore) {
                emit rechargeRecordsLoaded(recordList(records), hasMore);
            });
    connect(svc_, &charging::client::WalletService::operationFailed, this,
            [this](const QString& type, const charging::protocol::ProtocolError& error) {
                emit operationFailed(type, error.code, error.message);
            });
}

void WalletBridge::fetchProfile() { svc_->fetchProfile(); }
void WalletBridge::updateNickname(const QString& nickname) { svc_->updateNickname(nickname); }
void WalletBridge::updateAvatar(const QString& avatarKey) { svc_->updateAvatar(avatarKey); }
void WalletBridge::recharge(qint64 amountCents) { svc_->recharge(amountCents); }
void WalletBridge::fetchRechargeRecords(int page) { svc_->fetchRechargeRecords(page); }

// ————————————————————————————— OrderBridge —————————————————————————————

OrderBridge::OrderBridge(charging::client::OrderService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::OrderService::ordersLoaded, this,
            [this](const QVector<charging::client::OrderSummary>& orders, int total,
                   bool hasMore) {
                QVariantList out;
                out.reserve(orders.size());
                for (const auto& s : orders)
                    out.push_back(marshalling::orderToMap(s.order, s.stationName, s.chargerCode));
                emit ordersLoaded(out, total, hasMore);
            });
    connect(svc_, &charging::client::OrderService::statusCountsUpdated, this,
            &OrderBridge::statusCountsUpdated);
    connect(svc_, &charging::client::OrderService::operationFailed, this,
            [this](const QString& type, const charging::protocol::ProtocolError& error) {
                emit operationFailed(type, error.code, error.message);
            });
}

bool OrderBridge::isFetchingOrders() const { return svc_->isFetchingOrders(); }

void OrderBridge::fetchOrders(const QString& filter, int page)
{
    using F = charging::client::OrderService::Filter;    static const QHash<QString, F> map{
        {QStringLiteral("all"), F::All},
        {QStringLiteral("charging"), F::Charging},
        {QStringLiteral("waiting_payment"), F::WaitingPayment},
        {QStringLiteral("completed"), F::Completed},
    };
    svc_->fetchOrders(map.value(filter, F::All), page);
}

void OrderBridge::fetchStatusCounts() { svc_->fetchStatusCounts(); }

// ———————————————————————————— ChargingBridge ————————————————————————————

ChargingBridge::ChargingBridge(charging::client::ChargingService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::ChargingService::statusLoaded, this,
            [this](const charging::client::ChargingStatus& status) {
                emit statusLoaded(statusToMap(status));
            });
    connect(svc_, &charging::client::ChargingService::startCompleted, this,
            [this](const charging::client::ChargingStatus& status) {
                emit startCompleted(statusToMap(status));
            });
    connect(svc_, &charging::client::ChargingService::stopCompleted, this,
            [this](const charging::client::ChargingStatus& status) {
                emit stopCompleted(statusToMap(status));
            });
    connect(svc_, &charging::client::ChargingService::paymentCompleted, this,
            &ChargingBridge::paymentCompleted);
    connect(svc_, &charging::client::ChargingService::operationFailed, this,
            [this](const QString& type, const charging::protocol::ProtocolError& error) {
                emit operationFailed(type, error.code, error.message);
            });
}

void ChargingBridge::startTracking(qint64 orderId) { svc_->startTracking(orderId); }
void ChargingBridge::stopTracking() { svc_->stopTracking(); }
void ChargingBridge::fetchStatusNow() { svc_->fetchStatusNow(); }
void ChargingBridge::stopCharging() { svc_->stopCharging(); }
void ChargingBridge::startCharging(qint64 reservationId) { svc_->startCharging(reservationId); }
void ChargingBridge::payOrder(qint64 orderId) { svc_->payOrder(orderId); }

} // namespace charging::qml
