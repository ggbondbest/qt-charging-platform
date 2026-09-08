#include "service_bridges.h"

#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/coupon_service.h"
#include "charging/client/profile_charging/order_service.h"
#include "charging/client/profile_charging/stats_service.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"
#include "services/favorites/favorites_service.h"
#include "services/favorites/notification_service.h"
#include "services/reservation/reservation_service.h"
#include "services/settings/settings_service.h"
#include "services/station/station_query_service.h"

#include <QDateTime>
#include <QDate>
#include <QTime>
#include <QTimer>

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

// ————— 2026-09-07 补桥批的载荷翻译（枚举一律小写串，struct→map） —————

QString stationStatusWord(charging::model::StationStatus status)
{
    return status == charging::model::StationStatus::Active
        ? QStringLiteral("active") : QStringLiteral("inactive");
}

QString chargerTypeWord(charging::model::ChargerType type)
{
    return type == charging::model::ChargerType::Fast
        ? QStringLiteral("fast") : QStringLiteral("slow");
}

QString chargerStatusWord(charging::model::ChargerStatus status)
{
    switch (status) {
    case charging::model::ChargerStatus::Available: return QStringLiteral("available");
    case charging::model::ChargerStatus::Reserved:  return QStringLiteral("reserved");
    case charging::model::ChargerStatus::Charging:  return QStringLiteral("charging");
    case charging::model::ChargerStatus::Fault:     return QStringLiteral("fault");
    case charging::model::ChargerStatus::Offline:   return QStringLiteral("offline");
    }
    return QStringLiteral("available");
}

QString reservationStatusWord(charging::model::ReservationStatus status)
{
    switch (status) {
    case charging::model::ReservationStatus::Active:    return QStringLiteral("active");
    case charging::model::ReservationStatus::Fulfilled: return QStringLiteral("fulfilled");
    case charging::model::ReservationStatus::Cancelled: return QStringLiteral("cancelled");
    case charging::model::ReservationStatus::Expired:   return QStringLiteral("expired");
    }
    return QStringLiteral("active");
}

QString notificationTypeWord(
    charging::client::services::favorites::NotificationType type)
{
    switch (type) {
    case charging::client::services::favorites::NotificationType::ReservationExpiryReminder:
        return QStringLiteral("reservation_expiry_reminder");
    case charging::client::services::favorites::NotificationType::ReservationSuccessNotice:
        return QStringLiteral("reservation_success_notice");
    case charging::client::services::favorites::NotificationType::ReservationCancelNotice:
        return QStringLiteral("reservation_cancel_notice");
    case charging::client::services::favorites::NotificationType::ChargingStopped:
        return QStringLiteral("charging_stopped");
    case charging::client::services::favorites::NotificationType::OrderPaid:
        return QStringLiteral("order_paid");
    }
    return QStringLiteral("reservation_success_notice");
}

QVariantMap stationToMap(const charging::model::Station& station)
{
    return QVariantMap{
        {QStringLiteral("id"), station.id},
        {QStringLiteral("code"), station.code},
        {QStringLiteral("name"), station.name},
        {QStringLiteral("address"), station.address},
        {QStringLiteral("latitude"), station.latitude},
        {QStringLiteral("longitude"), station.longitude},
        {QStringLiteral("priceCentsPerKwh"), station.priceCentsPerKwh},
        {QStringLiteral("status"), stationStatusWord(station.status)},
        {QStringLiteral("totalChargers"), station.totalChargers},
        {QStringLiteral("availableChargers"), station.availableChargers},
    };
}

charging::model::Station stationFromMap(const QVariantMap& m)
{
    charging::model::Station s;
    s.id = m.value(QStringLiteral("id")).toLongLong();
    s.code = m.value(QStringLiteral("code")).toString();
    s.name = m.value(QStringLiteral("name")).toString();
    s.address = m.value(QStringLiteral("address")).toString();
    s.latitude = m.value(QStringLiteral("latitude")).toDouble();
    s.longitude = m.value(QStringLiteral("longitude")).toDouble();
    s.priceCentsPerKwh = m.value(QStringLiteral("priceCentsPerKwh")).toLongLong();
    s.status = m.value(QStringLiteral("status")).toString() == QStringLiteral("inactive")
        ? charging::model::StationStatus::Inactive : charging::model::StationStatus::Active;
    s.totalChargers = m.value(QStringLiteral("totalChargers")).toInt();
    s.availableChargers = m.value(QStringLiteral("availableChargers")).toInt();
    return s;
}

QVariantMap stationItemToMap(
    const charging::client::services::station::StationListItem& item)
{
    QVariantMap m = stationToMap(item.station);
    m.insert(QStringLiteral("distanceMeters"), item.distanceMeters);
    m.insert(QStringLiteral("operatorName"), item.operatorName);
    m.insert(QStringLiteral("accessType"), item.accessType);
    m.insert(QStringLiteral("parkingFee"), item.parkingFee);
    m.insert(QStringLiteral("features"), QVariantList(item.features.begin(), item.features.end()));
    m.insert(QStringLiteral("chargerTypes"),
             QVariantList(item.chargerTypes.begin(), item.chargerTypes.end()));
    m.insert(QStringLiteral("hasVoltageBelow700"), item.hasVoltageBelow700);
    m.insert(QStringLiteral("hasVoltageAtLeast700"), item.hasVoltageAtLeast700);
    return m;
}

QVariantMap chargerToMap(const charging::model::Charger& charger)
{
    return QVariantMap{
        {QStringLiteral("id"), charger.id},
        {QStringLiteral("stationId"), charger.stationId},
        {QStringLiteral("code"), charger.code},
        {QStringLiteral("type"), chargerTypeWord(charger.type)},
        {QStringLiteral("powerWatts"), charger.powerWatts},
        {QStringLiteral("status"), chargerStatusWord(charger.status)},
        {QStringLiteral("totalChargeCount"), charger.totalChargeCount},
        {QStringLiteral("totalChargeSeconds"), charger.totalChargeSeconds},
    };
}

QVariantMap stationDetailToMap(
    const charging::client::services::station::StationDetail& detail)
{
    QVariantMap m = stationToMap(detail.station);
    m.insert(QStringLiteral("distanceMeters"), detail.distanceMeters);
    m.insert(QStringLiteral("hasChargerData"), detail.hasChargerData);
    QVariantList chargers;
    chargers.reserve(detail.chargers.size());
    for (const auto& c : detail.chargers)
        chargers.push_back(chargerToMap(c));
    m.insert(QStringLiteral("chargers"), chargers);
    return m;
}

QVariantMap reservationRecordToMap(
    const charging::client::services::reservation::ReservationRecord& r)
{
    return QVariantMap{
        {QStringLiteral("id"), r.reservation.id},
        {QStringLiteral("reservationId"), r.reservation.id},
        {QStringLiteral("chargerId"), r.reservation.chargerId},
        {QStringLiteral("status"), reservationStatusWord(r.reservation.status)},
        {QStringLiteral("orderId"), r.orderId},
        {QStringLiteral("stationName"), r.stationName},
        {QStringLiteral("chargerCode"), r.chargerCode},
        {QStringLiteral("chargerSpec"), r.chargerSpec},
        {QStringLiteral("startAtUtc"), when(r.startAtUtc)},
        {QStringLiteral("expiresAtUtc"), when(r.reservation.expiresAtUtc)},
        {QStringLiteral("reservedAtUtc"), when(r.reservation.reservedAtUtc)},
        {QStringLiteral("endedAtUtc"), when(r.reservation.endedAtUtc)},
        {QStringLiteral("vehicleId"), r.vehicleId},
        {QStringLiteral("vehiclePlate"), r.vehiclePlate},
        {QStringLiteral("lateCancelled"), r.lateCancelled},
        {QStringLiteral("durationMinutes"), r.durationMinutes},
        {QStringLiteral("estimatedFeeCents"), r.estimatedFeeCents},
        {QStringLiteral("distanceMeters"), r.distanceMeters},
        {QStringLiteral("hasStationLocation"), r.hasStationLocation},
        {QStringLiteral("stationLatitude"), r.stationLatitude},
        {QStringLiteral("stationLongitude"), r.stationLongitude},
    };
}

QVariantMap vehicleToMap(
    const charging::client::services::settings::Vehicle& v)
{
    return QVariantMap{
        {QStringLiteral("id"), v.id},
        {QStringLiteral("plate"), v.plate},
        {QStringLiteral("brandModel"), v.brandModel},
        {QStringLiteral("batteryKwh"), v.batteryKwh},
        {QStringLiteral("connectorType"), chargerTypeWord(v.connectorType)},
        {QStringLiteral("isDefault"), v.isDefault},
    };
}

charging::client::services::settings::Vehicle vehicleFromMap(const QVariantMap& m)
{
    charging::client::services::settings::Vehicle v;
    v.id = m.value(QStringLiteral("id")).toLongLong();
    v.plate = m.value(QStringLiteral("plate")).toString();
    v.brandModel = m.value(QStringLiteral("brandModel")).toString();
    v.batteryKwh = m.value(QStringLiteral("batteryKwh")).toInt();
    v.connectorType = m.value(QStringLiteral("connectorType")).toString()
                          .compare(QLatin1String("slow"), Qt::CaseInsensitive) == 0
        ? charging::model::ChargerType::Slow : charging::model::ChargerType::Fast;
    v.isDefault = m.value(QStringLiteral("isDefault")).toBool();
    return v;
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

// ————————————————————————————— StationQueryBridge ————————————————————————————

StationQueryBridge::StationQueryBridge(
    charging::client::services::station::StationQueryService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::services::station::StationQueryService::queryStarted,
            this, &StationQueryBridge::queryStarted);
    connect(svc_, &charging::client::services::station::StationQueryService::querySucceeded,
            this, [this](const charging::client::services::station::StationList& stations) {
                QVariantList out;
                out.reserve(stations.size());
                stationCache_.clear();
                for (const auto& item : stations) {
                    const QVariantMap m = stationItemToMap(item);
                    stationCache_.insert(item.station.id, m);
                    out.push_back(m);
                }
                emit querySucceeded(out);
            });
    connect(svc_, &charging::client::services::station::StationQueryService::queryFailed,
            this, &StationQueryBridge::queryFailed);
    connect(svc_, &charging::client::services::station::StationQueryService::detailStarted,
            this, &StationQueryBridge::detailStarted);
    connect(svc_, &charging::client::services::station::StationQueryService::detailSucceeded,
            this, [this](const charging::client::services::station::StationDetail& detail) {
                const QVariantMap m = stationDetailToMap(detail);
                stationCache_.insert(detail.station.id, m);
                emit detailSucceeded(m);
            });
    connect(svc_, &charging::client::services::station::StationQueryService::detailFailed,
            this, &StationQueryBridge::detailFailed);
}

void StationQueryBridge::search(const QString& keyword) { svc_->search(keyword); }

void StationQueryBridge::fetchDetailById(qint64 stationId, int distanceMeters)
{
    // 详情页只带 id（列表 navigate 的 arg 可能不含全量字段）：优先用查询缓存
    // 重建 Station struct；缓存缺失时以 id 转发，服务端桩列表仍按 id 取。
    const QVariantMap cached = stationCache_.value(stationId);
    charging::model::Station station;
    if (!cached.isEmpty()) {
        station = stationFromMap(cached);
        if (distanceMeters < 0)
            distanceMeters = cached.value(QStringLiteral("distanceMeters"), -1).toInt();
    } else {
        station.id = stationId;
    }
    svc_->fetchDetail(station, distanceMeters);
}

bool StationQueryBridge::isQueryPending() const { return svc_->isQueryPending(); }

// ————————————————————————————— ReservationBridge —————————————————————————————

ReservationBridge::ReservationBridge(
    charging::client::services::reservation::ReservationService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::services::reservation::ReservationService::listStarted,
            this, &ReservationBridge::listStarted);
    connect(svc_, &charging::client::services::reservation::ReservationService::listSucceeded,
            this, [this](
                      const charging::client::services::reservation::ReservationList& records) {
                QVariantList out;
                out.reserve(records.size());
                for (const auto& r : records)
                    out.push_back(reservationRecordToMap(r));
                emit listSucceeded(out);
            });
    connect(svc_, &charging::client::services::reservation::ReservationService::listFailed,
            this, &ReservationBridge::listFailed);
    connect(svc_, &charging::client::services::reservation::ReservationService::submitStarted,
            this, &ReservationBridge::submitStarted);
    connect(svc_, &charging::client::services::reservation::ReservationService::submitSucceeded,
            this, [this](const charging::client::services::reservation::ReservationRecord& r) {
                emit submitSucceeded(reservationRecordToMap(r));
            });
    connect(svc_, &charging::client::services::reservation::ReservationService::submitFailed,
            this, &ReservationBridge::submitFailed);
    connect(svc_, &charging::client::services::reservation::ReservationService::cancelStarted,
            this, &ReservationBridge::cancelStarted);
    connect(svc_, &charging::client::services::reservation::ReservationService::cancelSucceeded,
            this, &ReservationBridge::cancelSucceeded);
    connect(svc_, &charging::client::services::reservation::ReservationService::cancelFailed,
            this, &ReservationBridge::cancelFailed);
    connect(svc_, &charging::client::services::reservation::ReservationService::reservationExpired,
            this, &ReservationBridge::reservationExpired);
}

void ReservationBridge::fetchList() { svc_->fetchList(); }

void ReservationBridge::submit(const QVariantMap& draft)
{
    // 与 widgets HomeShell 同口径：裸服务收完整 struct。桩的 code/type/power
    // 由确认页从详情 arg 透传（chargerCode 等键），缺失时 mock 记录展示空桩号。
    charging::model::Charger charger;
    charger.id = draft.value(QStringLiteral("chargerId")).toLongLong();
    charger.stationId = draft.value(QStringLiteral("stationId")).toLongLong();
    charger.code = draft.value(QStringLiteral("chargerCode")).toString();
    charger.type = draft.value(QStringLiteral("chargerType")).toString()
                       .compare(QLatin1String("slow"), Qt::CaseInsensitive) == 0
        ? charging::model::ChargerType::Slow : charging::model::ChargerType::Fast;
    charger.powerWatts = draft.value(QStringLiteral("chargerPowerWatts")).toInt();

    charging::model::Station station;
    station.id = charger.stationId;
    station.name = draft.value(QStringLiteral("stationName")).toString();

    // 确认页只传"当日分钟位"（本地时区，与推荐时段计算同基准）。桥据今天
    // 本地日期重建 QDateTime 再转 UTC；end<start 视为跨零点，end 顺延一天。
    const int startMin = draft.value(QStringLiteral("startMinutes")).toInt();
    const int endMin = draft.value(QStringLiteral("endMinutes")).toInt();
    const QDate today = QDate::currentDate();
    const QDateTime startLocal = QDateTime(
        today, QTime(0, 0).addMSecs(startMin * 60 * 1000));
    QDate endDate = today;
    if (endMin < startMin)
        endDate = today.addDays(1);
    const QDateTime endLocal = QDateTime(
        endDate, QTime(0, 0).addMSecs(endMin * 60 * 1000));

    svc_->submit(charger, station, startLocal.toUTC(), endLocal.toUTC(),
                 draft.value(QStringLiteral("vehicleId")).toLongLong(),
                 draft.value(QStringLiteral("vehiclePlate")).toString(),
                 draft.value(QStringLiteral("distanceMeters"), -1).toInt());
}

void ReservationBridge::cancel(qint64 reservationId) { svc_->cancel(reservationId); }
void ReservationBridge::expireReservation(qint64 reservationId)
{
    svc_->expireReservation(reservationId);
}
int ReservationBridge::cancelLateReservations() { return svc_->cancelLateReservations(); }
int ReservationBridge::activeReservationCount() const
{
    return svc_->activeReservationCount();
}
int ReservationBridge::unfinishedSlotLimit() const { return svc_->unfinishedSlotLimit(); }

// ————————————————————————————— SettingsBridge ——————————————————————————————

SettingsBridge::SettingsBridge(
    charging::client::services::settings::SettingsService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::services::settings::SettingsService::vehiclesChanged,
            this, &SettingsBridge::vehiclesChanged);
    connect(svc_, &charging::client::services::settings::SettingsService::protectionStateChanged,
            this, &SettingsBridge::protectionStateChanged);
    connect(svc_, &charging::client::services::settings::SettingsService::notificationsChanged,
            this, &SettingsBridge::notificationsChanged);
    connect(svc_, &charging::client::services::settings::SettingsService::appearanceChanged,
            this, &SettingsBridge::appearanceChanged);
}

QVariantList SettingsBridge::vehicles() const
{
    QVariantList out;
    for (const auto& v : svc_->vehicles())
        out.push_back(vehicleToMap(v));
    return out;
}

int SettingsBridge::vehicleCount() const { return svc_->vehicleCount(); }

qint64 SettingsBridge::addVehicle(const QVariantMap& vehicle)
{
    return svc_->addVehicle(vehicleFromMap(vehicle));
}

bool SettingsBridge::updateVehicle(const QVariantMap& vehicle)
{
    return svc_->updateVehicle(vehicleFromMap(vehicle));
}

bool SettingsBridge::removeVehicle(qint64 id) { return svc_->removeVehicle(id); }
void SettingsBridge::setDefaultVehicle(qint64 id) { svc_->setDefaultVehicle(id); }

bool SettingsBridge::hasSecondPassword() const { return svc_->hasProtectionPassword(); }
bool SettingsBridge::setSecondPassword(const QString& plain)
{
    return svc_->setProtectionPassword(plain);
}
bool SettingsBridge::verifySecondPassword(const QString& plain) const
{
    return svc_->verifyProtectionPassword(plain);
}
bool SettingsBridge::protectionEnabled() const { return svc_->protectionEnabled(); }
bool SettingsBridge::setSecondProtectionEnabled(bool enabled)
{
    return svc_->setProtectionEnabled(enabled);
}

namespace {
charging::client::services::settings::SettingsService::Notification
notificationFromKey(const QString& key)
{
    using Settings = charging::client::services::settings::SettingsService;
    if (key == QLatin1String("success"))
        return Settings::Notification::ReservationSuccessNotice;
    if (key == QLatin1String("cancel"))
        return Settings::Notification::ReservationCancelNotice;
    return Settings::Notification::ReservationExpiryReminder;   // "expiry"/未知兜底
}
} // namespace

bool SettingsBridge::notificationEnabled(const QString& key) const
{
    return svc_->notificationEnabled(notificationFromKey(key));
}

void SettingsBridge::setNotificationEnabled(const QString& key, bool enabled)
{
    svc_->setNotificationEnabled(notificationFromKey(key), enabled);
}

QString SettingsBridge::theme() const { return svc_->theme(); }
bool SettingsBridge::setTheme(const QString& theme) { return svc_->setTheme(theme); }
QString SettingsBridge::fontScale() const { return svc_->fontScale(); }
bool SettingsBridge::setFontScale(const QString& scale) { return svc_->setFontScale(scale); }

// ————————————————————————————— FavoritesBridge ——————————————————————————————

FavoritesBridge::FavoritesBridge(
    charging::client::services::favorites::FavoritesService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::services::favorites::FavoritesService::favoritesChanged,
            this, &FavoritesBridge::favoritesChanged);
}

bool FavoritesBridge::contains(qint64 stationId) const { return svc_->contains(stationId); }
bool FavoritesBridge::toggle(qint64 stationId) { return svc_->toggle(stationId); }

QVariantList FavoritesBridge::favoriteIds() const
{
    const QVector<qint64> ids = svc_->favoriteIds();
    return QVariantList(ids.begin(), ids.end());
}

// ———————————————————————————— NotificationBridge ————————————————————————————

NotificationBridge::NotificationBridge(
    charging::client::services::favorites::NotificationService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::services::favorites::NotificationService::notificationsChanged,
            this, &NotificationBridge::notificationsChanged);
}

QVariantList NotificationBridge::notifications() const
{
    QVariantList out;
    for (const auto& item : svc_->notifications()) {
        out.push_back(QVariantMap{
            {QStringLiteral("id"), item.id},
            {QStringLiteral("type"), notificationTypeWord(item.type)},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("body"), item.body},
            {QStringLiteral("createdAtUtc"), when(item.createdAtUtc)},
        });
    }
    return out;
}

// ———————————————————————————— StatsBridge / CouponBridge ————————————————————————————

StatsBridge::StatsBridge(charging::client::StatsService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::StatsService::statsLoaded,
            this, &StatsBridge::statsLoaded);
    connect(svc_, &charging::client::StatsService::operationFailed, this,
            [this](const QString& type, const charging::protocol::ProtocolError& error) {
                emit operationFailed(type, error.code, error.message);
            });
}

void StatsBridge::fetchStats(int months) { svc_->fetchStats(months); }
bool StatsBridge::isFetchingStats() const { return svc_->isFetchingStats(); }

CouponBridge::CouponBridge(charging::client::CouponService* svc, QObject* parent)
    : QObject(parent), svc_(svc)
{
    connect(svc_, &charging::client::CouponService::couponsChanged,
            this, &CouponBridge::couponsChanged);
    connect(svc_, &charging::client::CouponService::operationFailed, this,
            [this](const QString& type, const charging::protocol::ProtocolError& error) {
                emit operationFailed(type, error.code, error.message);
            });
    // No self-warm here: an eager GET_COUPONS would consume the mock's
    // scripted setNextFailure sequences and race QSignalSpy starts. The page
    // pulls on entry (Component.onCompleted fetchCoupons) instead.
}

QVariantList CouponBridge::coupons() const { return svc_->coupons(); }
void CouponBridge::fetchCoupons() { svc_->fetchCoupons(); }
int CouponBridge::couponCount() const { return static_cast<int>(svc_->coupons().size()); }


} // namespace charging::qml
