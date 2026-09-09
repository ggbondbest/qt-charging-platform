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

#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "charging/common/model/enums.h"

namespace charging::client {
class WalletService;
class OrderService;
class ChargingService;
class StatsService;
class CouponService;
class PointService;
class RatingService;
struct OrderSummary;
namespace services::station { class StationQueryService; }
namespace services::reservation { class ReservationService; }
namespace services::settings { class SettingsService; }
namespace services::favorites { class FavoritesService; class NotificationService; }
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
    Q_INVOKABLE bool isFetchingRecords() const;
    Q_INVOKABLE bool isUpdatingProfile() const;

signals:
    void profileLoaded(const QVariantMap& user);
    void profileUpdated(const QString& field, const QVariantMap& user);
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

    Q_INVOKABLE void startTracking(const QVariant& orderId);
    Q_INVOKABLE void stopTracking();
    Q_INVOKABLE void fetchStatusNow();
    Q_INVOKABLE void stopCharging();
    Q_INVOKABLE void startCharging(const QVariant& reservationId);
    Q_INVOKABLE void startChargingWithTarget(const QVariant& reservationId,
                                            const QString& type, double value);
    Q_INVOKABLE bool isStarting() const;
    Q_INVOKABLE void payOrder(const QVariant& orderId);

signals:
    void statusLoaded(const QVariantMap& status);
    void startCompleted(const QVariantMap& status);
    void stopCompleted(const QVariantMap& status);
    void paymentCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::ChargingService* svc_;
};

// ————— 2026-09-07 补桥批（PR #36 落地清单）：station/reservation/settings/
// favorites/notification。方法名/信号名与裸服务逐字一致（CONTRACT §1），仅把
// 裸 struct 载荷翻成 QML 可读的 map/list，枚举参数走小写字符串。
// 唯一例外：fetchDetailById 是**新增桥方法**（裸服务只收 Station struct，
// QML 传不动 struct）；以及 SettingsBridge 的 secondPassword 别名对
// （成员2 页面按 UI 语义用 second*，裸服务叫 protection*）。

class StationQueryBridge final : public QObject
{
    Q_OBJECT
public:
    explicit StationQueryBridge(charging::client::services::station::StationQueryService* svc,
                                QObject* parent = nullptr);

    Q_INVOKABLE void search(const QString& keyword = QString());
    // 桥侧新法：按 id 从上次查询结果重建 Station struct 再转发 fetchDetail。
    Q_INVOKABLE void fetchDetailById(const QVariant& stationId, int distanceMeters = -1);
    Q_INVOKABLE bool isQueryPending() const;

signals:
    void queryStarted();
    void querySucceeded(const QVariantList& stations);
    void queryFailed(const QString& message);
    void detailStarted();
    void detailSucceeded(const QVariantMap& detail);
    void detailFailed(const QString& message);

private:
    charging::client::services::station::StationQueryService* svc_;
    QHash<qint64, QVariantMap> stationCache_;   // id → flattened list item
};

class ReservationBridge final : public QObject
{
    Q_OBJECT
public:
    explicit ReservationBridge(charging::client::services::reservation::ReservationService* svc,
                               QObject* parent = nullptr);

    Q_INVOKABLE void fetchList();
    // 载荷 {chargerId, stationId, chargerCode, chargerType("fast"|"slow"),
    //       chargerPowerWatts, stationName, startMinutes, endMinutes,
    //       vehicleId, vehiclePlate, distanceMeters}。startMinutes/endMinutes
    //       为**当日分钟位**（本地，与推荐时段同基准），桥据今日重建 QDateTime
    //       再转 UTC；end<start 视作跨零点顺延一天（与 widgets QDateTimeEdit 口径一致）。
    Q_INVOKABLE void submit(const QVariantMap& draft);
    Q_INVOKABLE void cancel(const QVariant& reservationId);
    Q_INVOKABLE void expireReservation(const QVariant& reservationId);
    Q_INVOKABLE int cancelLateReservations();
    Q_INVOKABLE int activeReservationCount() const;
    Q_INVOKABLE int unfinishedSlotLimit() const;

signals:
    void listStarted();
    void listSucceeded(const QVariantList& records);
    void listFailed(const QString& message);
    void submitStarted(const QString& chargerId);
    void submitSucceeded(const QVariantMap& record);
    void submitFailed(const QString& reason);
    void submitRejected(const QString& code, const QVariantMap& details, const QString& message);
    void cancelStarted(const QString& reservationId);
    void cancelSucceeded(const QString& reservationId);
    void cancelFailed(const QString& message);
    void reservationExpired(const QString& reservationId);

private:
    charging::client::services::reservation::ReservationService* svc_;
};

class SettingsBridge final : public QObject
{
    Q_OBJECT
public:
    explicit SettingsBridge(charging::client::services::settings::SettingsService* svc,
                            QObject* parent = nullptr);

    Q_INVOKABLE QVariantList vehicles() const;
    Q_INVOKABLE int vehicleCount() const;
    Q_INVOKABLE QString addVehicle(const QVariantMap& vehicle);
    Q_INVOKABLE bool updateVehicle(const QVariantMap& vehicle);
    Q_INVOKABLE bool removeVehicle(const QVariant& id);
    Q_INVOKABLE void setDefaultVehicle(const QVariant& id);
    // second* 别名 → 裸服务 protection*（语义同一：二级保护密码）。
    Q_INVOKABLE bool hasSecondPassword() const;
    Q_INVOKABLE bool setSecondPassword(const QString& plain);
    Q_INVOKABLE bool verifySecondPassword(const QString& plain) const;
    Q_INVOKABLE bool protectionEnabled() const;
    Q_INVOKABLE bool setSecondProtectionEnabled(bool enabled);
    // key ∈ "expiry" | "success" | "cancel"
    Q_INVOKABLE bool notificationEnabled(const QString& key) const;
    Q_INVOKABLE void setNotificationEnabled(const QString& key, bool enabled);
    // 外观（2026-09-08 批次A）：值域即 Service 白名单——
    // theme ∈ "light" | "dark"；fontScale ∈ "standard" | "large" | "extraLarge"。
    Q_INVOKABLE QString theme() const;
    Q_INVOKABLE bool setTheme(const QString& theme);
    Q_INVOKABLE QString fontScale() const;
    Q_INVOKABLE bool setFontScale(const QString& scale);

signals:
    void vehiclesChanged();
    void protectionStateChanged();
    void notificationsChanged();
    void appearanceChanged();   // 主题/字号任一变更（批次A，Shell 据此重同步 Style）

private:
    charging::client::services::settings::SettingsService* svc_;
};

class FavoritesBridge final : public QObject
{
    Q_OBJECT
public:
    explicit FavoritesBridge(charging::client::services::favorites::FavoritesService* svc,
                             QObject* parent = nullptr);

    Q_INVOKABLE bool contains(const QVariant& stationId) const;
    Q_INVOKABLE bool toggle(const QVariant& stationId);
    Q_INVOKABLE QVariantList favoriteIds() const;

signals:
    void favoritesChanged();

private:
    charging::client::services::favorites::FavoritesService* svc_;
};

class NotificationBridge final : public QObject
{
    Q_OBJECT
public:
    explicit NotificationBridge(
        charging::client::services::favorites::NotificationService* svc,
        QObject* parent = nullptr);

    // [{id, type("reservation_expiry_reminder"|"reservation_success_notice"
    //     |"reservation_cancel_notice"|"charging_stopped"|"order_paid"),
    //   title, body, createdAtUtc}] 新→旧。服务端通道类型由
    //   NotificationService::refresh() 拉入（2026-09-08）。
    Q_INVOKABLE QVariantList notifications() const;

signals:
    void notificationsChanged();

private:
    charging::client::services::favorites::NotificationService* svc_;
};

// ————— 2026-09-08 月报/优惠券桥（成员3 新页 + 成员2 CouponPage 盲调退演示态）。

class StatsBridge final : public QObject
{
    Q_OBJECT
public:
    explicit StatsBridge(charging::client::StatsService* svc, QObject* parent = nullptr);

    Q_INVOKABLE void fetchStats(int months = 6,
                                const QString& period = QStringLiteral("month"));
    Q_INVOKABLE bool isFetchingStats() const;

signals:
    // [{monthKey, orderCount, energyWh, amountCents, durationSeconds, co2Grams}] 新→旧
    void statsLoaded(const QVariantList& months);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::StatsService* svc_;
};

class CouponBridge final : public QObject
{
    Q_OBJECT
public:
    explicit CouponBridge(charging::client::CouponService* svc, QObject* parent = nullptr);

    // CouponPage 契约：[{id,title,kind,valueCents,discountTenths,thresholdCents,
    //                   condition,expiresAtUtc(ms),status,source}] 新→旧。
    // 页面进入时 fetchCoupons() 拉取、couponsChanged 后 coupons() 取缓存。
    // redeem 一期不提供（TODO(contract): PAY_ORDER 抵扣规则）。
    Q_INVOKABLE QVariantList coupons() const;
    Q_INVOKABLE void fetchCoupons();
    Q_INVOKABLE int couponCount() const;

signals:
    void couponsChanged();
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::CouponService* svc_;
};

// ————— 2026-09-08 批次C：签到/积分桥（成员3 新页 StatsPage 同目录的
// PointsPage 消费；context property 名 pointsService）。

class PointBridge final : public QObject
{
    Q_OBJECT
public:
    explicit PointBridge(charging::client::PointService* svc, QObject* parent = nullptr);

    Q_INVOKABLE bool isBusy() const;
    Q_INVOKABLE void fetchPoints(int page = 1, int pageSize = 20);
    Q_INVOKABLE void checkIn();

signals:
    // entries 行 = GET_POINTS 响应形 [{id,amount,reason,createdAtUtc}] 新→旧
    void pointsLoaded(qint64 points, const QVariantList& entries, int total);
    void checkInCompleted(const QString& day, qint64 points, qint64 gained,
                          bool alreadyCheckedIn);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::PointService* svc_;
};

// ————— 2026-09-08 批次E：电桩评价桥（RatingsPage + OrderDetailPage 评价卡
// 消费；context property 名 ratingsService）。

class RatingBridge final : public QObject
{
    Q_OBJECT
public:
    explicit RatingBridge(charging::client::RatingService* svc, QObject* parent = nullptr);

    Q_INVOKABLE bool isBusy() const;
    Q_INVOKABLE void fetchMyRatings(int page = 1, int pageSize = 20);
    // orderId 须正十进制串（服务端 normalize 形态校验）；rating 1..5。
    Q_INVOKABLE void submitRating(const QString& orderId, int rating, const QString& comment);

signals:
    // ratings 行 = GET_MY_RATINGS 响应形 [{id,orderId,chargerId,chargerCode,
    // stationName,rating,comment,createdAtUtc}] 新→旧
    void ratingsLoaded(const QVariantList& ratings, int total);
    // 提交成功与幂等重放都发；ratingRow = 服务端回读行（重放=首评原值）
    void ratingSubmitted(const QVariantMap& ratingRow, bool alreadyRated);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    charging::client::RatingService* svc_;
};

} // namespace charging::qml
