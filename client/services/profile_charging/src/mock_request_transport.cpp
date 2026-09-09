#include "charging/client/profile_charging/mock_request_transport.h"

#include "charging/common/model/enums.h"
#include "charging/common/model/model_json.h"
#include "charging/common/protocol/protocol.h"
#include "charging/common/protocol/user_api_contract.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace charging::client {

namespace {

constexpr int kMockLatencyMs = 450;

// Assumed live output of the demo charger while an order is Charging.
constexpr qint64 kMockLivePowerWatts = 6500;

charging::protocol::ProtocolError mockError(const QString& code, const QString& message)
{
    charging::protocol::ProtocolError error;
    error.code = code;
    error.message = message;
    return error;
}

// 契约 v1 §3（UPDATE_USER_INFO）：非空 avatarKey 必须属于内置头像清单，格式校验
// 之外由"服务端"做清单检查。此清单必须与 server/services/user_api_service.cpp 及
// AvatarLibrary::all() 完全一致（tst_wallet_service 有对拍测试钉住三方）。
bool isBuiltInAvatarKey(const QString& key)
{
    static const QStringList keys{QStringLiteral("bolt"), QStringLiteral("plug"),
                                  QStringLiteral("car"), QStringLiteral("leaf"),
                                  QStringLiteral("cat"), QStringLiteral("panda"),
                                  QStringLiteral("moon"), QStringLiteral("rocket")};
    return key.isEmpty() || keys.contains(key);
}

QDateTime isoUtc(const QString& text)
{
    return QDateTime::fromString(text, QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
}

qint64 amountFor(qint64 energyWh, qint64 priceCentsPerKwh)
{
    // Same integer billing math as the server: Wh * cents/kWh / 1000, half-up.
    return (energyWh * priceCentsPerKwh + 500) / 1000;
}

} // namespace

MockRequestTransport::MockRequestTransport()
{
    seedDemoData();
    seedDemoOrders();
    seedDemoCoupons();
    seedDemoPoints();
    seedDemoRatings();   // 依赖 orders_，必须排在 seedDemoOrders 之后。
}

void MockRequestTransport::seedDemoData()
{
    // Mirrors the seed demo account: phone 13800138000, balance 100.00 yuan.
    user_.id = 1;
    user_.phone = QStringLiteral("13800138000");
    user_.nickname = QStringLiteral("用户8000");
    user_.avatarKey = QString();
    user_.balanceCents = 10000;
    user_.status = charging::model::UserStatus::Active;
    user_.createdAtUtc = isoUtc(QStringLiteral("2026-08-01T00:00:00.000Z"));
    user_.updatedAtUtc = isoUtc(QStringLiteral("2026-08-28T09:12:00.000Z"));

    struct SeedRecord
    {
        qint64 amountCents;
        qint64 balanceAfterCents;
        QString createdAt;
        charging::model::RechargeStatus status;
    };
    // 真实服务器 id 自增即时间序，"id 倒序"才等价"最新在前"；种子必须按时间
    // 升序灌入让 id 与时间同向。余额快照链也要闭合：0+20=20、+50=70、+30=100，
    // 末笔快照恰等于演示余额 100.00 元。
    const QVector<SeedRecord> seeds = {
        {2000, 2000, QStringLiteral("2026-08-05T07:55:00.000Z"),
         charging::model::RechargeStatus::Success},
        {5000, 7000, QStringLiteral("2026-08-20T12:41:00.000Z"),
         charging::model::RechargeStatus::Success},
        {3000, 10000, QStringLiteral("2026-08-28T09:12:00.000Z"),
         charging::model::RechargeStatus::Success},
    };
    for (const SeedRecord& seed : seeds) {
        charging::model::RechargeRecord record;
        record.id = nextRecordId_++;
        record.transactionNo =
            QStringLiteral("MOCKRCH%1").arg(nextTransactionSeq_++, 8, 10, QChar('0'));
        record.userId = user_.id;
        record.amountCents = seed.amountCents;
        record.balanceAfterCents = seed.balanceAfterCents;
        record.status = seed.status;
        record.createdAtUtc = isoUtc(seed.createdAt);
        records_.prepend(record); // keep newest first
    }
}

void MockRequestTransport::seedDemoOrders()
{
    // Station/charger display strings the list and detail pages show. The
    // real server is expected to join them into the GET_ORDERS payload; the
    // mock keeps the same mapping.
    chargerDisplays_.insert(11, {QStringLiteral("云杉科技园区充电站"), QStringLiteral("A03")});
    chargerDisplays_.insert(12, {QStringLiteral("云杉科技园区充电站"), QStringLiteral("B07")});
    chargerDisplays_.insert(21, {QStringLiteral("万达广场地库充电站"), QStringLiteral("C02")});

    const QDateTime now = QDateTime::currentDateTimeUtc();
    int orderSeq = 1;

    auto makeOrder = [&now, &orderSeq](qint64 chargerId, charging::model::OrderStatus status,
                                       qint64 priceCentsPerKwh, qint64 energyWh,
                                       qint64 durationSeconds, qint64 startedAgoSeconds,
                                       qint64 stoppedAgoSeconds, qint64 paidAgoSeconds) {
        charging::model::Order order;
        order.id = orderSeq;
        order.orderNo = QStringLiteral("MOCKORD%1")
                            .arg(now.toUTC().toString(QStringLiteral("yyyyMMdd")) +
                                     QStringLiteral("%1").arg(orderSeq++, 4, 10, QChar('0')));
        order.userId = 1;
        order.chargerId = chargerId;
        order.reservationId = 0;
        order.status = status;
        order.unitPriceCentsPerKwh = priceCentsPerKwh;
        order.energyWh = energyWh;
        order.durationSeconds = durationSeconds;
        order.amountCents = amountFor(energyWh, priceCentsPerKwh);
        order.createdAtUtc = startedAgoSeconds > 0
                                 ? now.addSecs(-startedAgoSeconds - 60)
                                 : now.addSecs(-stoppedAgoSeconds - 60);
        if (startedAgoSeconds > 0) {
            order.startedAtUtc = now.addSecs(-startedAgoSeconds);
        }
        if (stoppedAgoSeconds > 0) {
            order.stoppedAtUtc = now.addSecs(-stoppedAgoSeconds);
        }
        if (paidAgoSeconds > 0) {
            order.paidAtUtc = now.addSecs(-paidAgoSeconds);
        }
        order.updatedAtUtc = order.paidAtUtc.isValid() ? order.paidAtUtc
                       : (order.stoppedAtUtc.isValid() ? order.stoppedAtUtc
                                                       : order.createdAtUtc);
        return order;
    };

    // Oldest first; reversed below so orders_ ends up newest first.
    // 种子时间刻意跨 3 个自然月（约 70/40 天前 + 本周），让列表的月度分组
    // 表头在预览里始终可见；相对"今天"计算以免种子过期。
    orders_.append(makeOrder(12, charging::model::OrderStatus::Cancelled, 98, 0, 0,
                             0, 0, 0));
    orders_[orders_.size() - 1].createdAtUtc = now.addDays(-71);
    orders_[orders_.size() - 1].updatedAtUtc = now.addDays(-71);
    orders_[orders_.size() - 1].amountCents = 0;

    orders_.append(makeOrder(11, charging::model::OrderStatus::Completed, 132, 24680, 5538,
                             40 * 24 * 3600, 40 * 24 * 3600 - 5538, 40 * 24 * 3600 - 5538 - 60));
    orders_.append(makeOrder(12, charging::model::OrderStatus::Completed, 98, 15200, 3300,
                             24 * 3600 + 3600, 24 * 3600, 24 * 3600 - 30));
    orders_.append(makeOrder(11, charging::model::OrderStatus::WaitingPayment, 132, 18500, 4210,
                             5 * 3600, 3 * 3600, 0));
    // The CHARGING order carries a stored snapshot; the real-time values come
    // from GET_CHARGING_STATUS once the leader freezes that payload.
    orders_.append(makeOrder(21, charging::model::OrderStatus::Charging, 150, 6210, 1860,
                             31 * 60, 0, 0));

    std::reverse(orders_.begin(), orders_.end());
}

void MockRequestTransport::seedDemoCoupons()
{
    // 与 CouponPage.qml 内置演示数据同 shape（桥注册后页面自动退演示态）。
    // 字段即 GET_COUPONS 响应形；id 用服务端同款十进制字符串，expiresAtUtc 毫秒数。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 dayMs = 86400000;
    const auto coupon = [](qint64 id, const QString& kind, const QString& title,
                           qint64 valueCents, int discountTenths, const QString& condition,
                           qint64 expiresAtUtc, const QString& status, const QString& source) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), QString::number(id));
        object.insert(QStringLiteral("kind"), kind);
        object.insert(QStringLiteral("title"), title);
        if (kind == QLatin1String("cash")) {
            object.insert(QStringLiteral("valueCents"), static_cast<double>(valueCents));
        } else {
            object.insert(QStringLiteral("discountTenths"), discountTenths);
        }
        object.insert(QStringLiteral("thresholdCents"), 0);
        object.insert(QStringLiteral("condition"), condition);
        object.insert(QStringLiteral("expiresAtUtc"), static_cast<double>(expiresAtUtc));
        object.insert(QStringLiteral("status"), status);
        object.insert(QStringLiteral("source"), source);
        return object;
    };
    coupons_ = {
        coupon(1, QStringLiteral("cash"), QStringLiteral("新客立减券"), 1000, 0,
               QStringLiteral("充电满 ¥20 可用"), nowMs + 6 * dayMs,
               QStringLiteral("available"), QStringLiteral("平台新客礼")),
        coupon(2, QStringLiteral("discount"), QStringLiteral("充电折扣券"), 0, 88,
               QStringLiteral("单笔最高抵 ¥5"), nowMs + 3 * dayMs,
               QStringLiteral("available"), QStringLiteral("充值回馈")),
        coupon(3, QStringLiteral("cash"), QStringLiteral("服务费减免券"), 500, 0,
               QStringLiteral("服务费满 ¥5 可用"), nowMs + 14 * dayMs,
               QStringLiteral("available"), QStringLiteral("活动发放")),
        coupon(4, QStringLiteral("cash"), QStringLiteral("节日充电券"), 2000, 0,
               QStringLiteral("满 ¥30 可用"), nowMs - 2 * dayMs,
               QStringLiteral("used"), QStringLiteral("节日活动")),
        coupon(5, QStringLiteral("cash"), QStringLiteral("早鸟体验券"), 800, 0,
               QStringLiteral("无门槛"), nowMs - 5 * dayMs,
               QStringLiteral("expired"), QStringLiteral("拉新奖励")),
    };
}

void MockRequestTransport::appendMockNotification(const QString& type, const QString& title,
                                                  const QString& body,
                                                  const QDateTime& createdAtUtc)
{
    notifications_.prepend({type, title, body, createdAtUtc});
}

void MockRequestTransport::grantRechargeCoupon(qint64 amountCents, const QDateTime& nowUtc)
{
    // 镜像服务端规则：单笔充值 ≥ ¥50 送一张 ¥5 充电券、30 天有效
    //（常量单点在 user_api_contract.h，TODO(contract): 业务终确认待定稿）。
    if (amountCents < charging::protocol::user_api::kCouponRechargeThresholdCents) {
        return;
    }
    QJsonObject object;
    object.insert(QStringLiteral("id"), QString::number(nextCouponId_++));
    object.insert(QStringLiteral("kind"), QStringLiteral("cash"));
    object.insert(QStringLiteral("title"), QStringLiteral("充值回馈 ¥5 充电券"));
    object.insert(QStringLiteral("valueCents"),
                  static_cast<double>(charging::protocol::user_api::kCouponValueCents));
    object.insert(QStringLiteral("thresholdCents"), 0);
    object.insert(QStringLiteral("condition"), QStringLiteral("无门槛"));
    object.insert(QStringLiteral("expiresAtUtc"),
                  static_cast<double>(nowUtc.addDays(
                      charging::protocol::user_api::kCouponValidityDays).toMSecsSinceEpoch()));
    object.insert(QStringLiteral("status"), QStringLiteral("available"));
    object.insert(QStringLiteral("source"), QStringLiteral("充值回馈"));
    coupons_.prepend(object);
}

void MockRequestTransport::seedDemoPoints()
{
    // 批次C 演示流水：注册礼包 +50（3 天前）。不预置今日签到——签到按钮
    // 的首演留给 CHECK_IN 本身（与真机语义一致：日粒度幂等由 checkInDay_ 记）。
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject welcome;
    welcome.insert(QStringLiteral("id"), QString::number(nextLedgerId_++));
    welcome.insert(QStringLiteral("amount"), 50);
    welcome.insert(QStringLiteral("reason"), QStringLiteral("注册礼包"));
    welcome.insert(QStringLiteral("createdAtUtc"),
                   now.addDays(-3).toUTC().toString(Qt::ISODateWithMs));
    pointsLedger_.append(welcome);
    pointsTotal_ += 50;
}

void MockRequestTransport::seedDemoRatings()
{
    // 批次E 演示：给最近一笔已完成订单预置一条五星评价，让"我的评价"页与
    // OrderDetail 完成态首演即有内容。幂等锚 order_id UNIQUE——同一订单只此一条。
    for (const charging::model::Order& order : orders_) {
        if (order.status != charging::model::OrderStatus::Completed) {
            continue;
        }
        const QString orderId = QString::number(order.id);
        QJsonObject object;
        object.insert(QStringLiteral("id"), QString::number(nextRatingId_++));
        object.insert(QStringLiteral("orderId"), orderId);
        object.insert(QStringLiteral("chargerId"), QString::number(order.chargerId));
        const auto display = chargerDisplays_.constFind(order.chargerId);
        if (display != chargerDisplays_.constEnd()) {
            object.insert(QStringLiteral("stationName"), display->first);
            object.insert(QStringLiteral("chargerCode"), display->second);
        }
        object.insert(QStringLiteral("rating"), 5);
        object.insert(QStringLiteral("comment"), QStringLiteral("充电很快，环境不错。"));
        object.insert(QStringLiteral("createdAtUtc"),
                      order.updatedAtUtc.toUTC().toString(Qt::ISODateWithMs));
        ratings_.prepend(object);
        break;   // 只预置一条，其余留给真实提交演示。
    }
}

void MockRequestTransport::setNextFailure(const QString& code, int times)
{
    nextFailureCode_ = code;
    nextFailureRemaining_ = times > 0 ? times : 1;
}

void MockRequestTransport::drainBalanceTo(qint64 cents)
{
    user_.balanceCents = cents > 0 ? cents : 0;
    user_.updatedAtUtc = QDateTime::currentDateTimeUtc();
}

void MockRequestTransport::setUser(const charging::model::User& user)
{
    // Seed the real login account; demo orders/records stay (see header note).
    user_.id = user.id;
    user_.phone = user.phone;
    user_.nickname = user.nickname;
    user_.avatarKey = user.avatarKey;
    user_.balanceCents = user.balanceCents;
    user_.status = user.status;
    user_.updatedAtUtc = QDateTime::currentDateTimeUtc();
}

void MockRequestTransport::cancelActiveOrders()
{
    for (charging::model::Order& order : orders_) {
        if (order.status != charging::model::OrderStatus::Completed
            && order.status != charging::model::OrderStatus::Cancelled) {
            order.status = charging::model::OrderStatus::Cancelled;
            order.updatedAtUtc = QDateTime::currentDateTimeUtc();
        }
    }
}

void MockRequestTransport::registerMockReservation(qint64 reservationId, qint64 chargerId,
                                                   const QString& stationName,
                                                   const QString& chargerCode)
{
    mockReservations_.insert(reservationId, {chargerId, {stationName, chargerCode}});
}

charging::model::Order* MockRequestTransport::findOrder(qint64 orderId)
{
    for (charging::model::Order& order : orders_) {
        if (order.id == orderId) {
            return &order;
        }
    }
    return nullptr;
}

QJsonObject MockRequestTransport::buildStatusPayload(const charging::model::Order& order,
                                                     qint64 powerWatts, qint64 energyWh,
                                                     qint64 durationSeconds) const
{
    // Mirrors the frozen contract (docs/api/socket_protocol.md §8.4/8.5):
    // the order snapshot (live durationSeconds/energyWh/amountCents for a
    // CHARGING order, persisted values otherwise) travels under `order`,
    // and the live power is a sibling `currentPowerWatts` (0 when the order
    // is not charging). stationName/chargerCode stay siblings pending the
    // leader's join decision (TODO(contract): not part of §8.4 yet).
    QJsonObject orderObject = charging::model::toJson(order);
    orderObject.insert(QStringLiteral("energyWh"), energyWh);
    orderObject.insert(QStringLiteral("durationSeconds"), durationSeconds);
    orderObject.insert(QStringLiteral("amountCents"),
                       amountFor(energyWh, order.unitPriceCentsPerKwh));

    QJsonObject payload;
    payload.insert(QStringLiteral("order"), orderObject);
    payload.insert(QStringLiteral("currentPowerWatts"), powerWatts);
    const auto display = chargerDisplays_.constFind(order.chargerId);
    if (display != chargerDisplays_.constEnd()) {
        payload.insert(QStringLiteral("stationName"), display->first);
        payload.insert(QStringLiteral("chargerCode"), display->second);
    }
    return payload;
}

QJsonObject MockRequestTransport::payResultPayload(const charging::model::Order& order) const
{
    // §8.6: PAY_ORDER success carries the (completed) order object plus the
    // post-payment authoritative balance under `balanceCents`.
    QJsonObject payload;
    payload.insert(QStringLiteral("order"), charging::model::toJson(order));
    payload.insert(QStringLiteral("balanceCents"), user_.balanceCents);
    return payload;
}

void MockRequestTransport::send(const QString& type, const QJsonObject& data,
                                const ResponseCallback& callback)
{
    // The production transport will correlate on this requestId; the mock
    // keeps the shape only.
    const QUuid requestId = QUuid::createUuid();
    Q_UNUSED(requestId);

    QTimer::singleShot(kMockLatencyMs, this,
                       [this, type, data, callback]() { handleRequest(type, data, callback); });
}

void MockRequestTransport::handleRequest(const QString& type, const QJsonObject& data,
                                         const ResponseCallback& callback)
{
    if (nextFailureRemaining_ > 0) {
        --nextFailureRemaining_;
        callback(false, QJsonObject{}, mockError(nextFailureCode_,
                                                 QStringLiteral("simulated transport failure")));
        return;
    }

    const QString getUserInfoType =
        QString::fromLatin1(charging::protocol::request_type::kGetUserInfo);
    const QString updateUserInfoType =
        QString::fromLatin1(charging::protocol::request_type::kUpdateUserInfo);
    const QString rechargeType =
        QString::fromLatin1(charging::protocol::request_type::kRecharge);
    const QString getRecordsType =
        QString::fromLatin1(charging::protocol::request_type::kGetRechargeRecords);
    const QString getOrdersType =
        QString::fromLatin1(charging::protocol::request_type::kGetOrders);
    const QString getChargingStatusType =
        QString::fromLatin1(charging::protocol::request_type::kGetChargingStatus);
    const QString stopChargingType =
        QString::fromLatin1(charging::protocol::request_type::kStopCharging);
    const QString startChargingType =
        QString::fromLatin1(charging::protocol::request_type::kStartCharging);
    const QString payOrderType =
        QString::fromLatin1(charging::protocol::request_type::kPayOrder);
    const QString getUserStatsType =
        QString::fromLatin1(charging::protocol::request_type::kGetUserStats);
    const QString getCouponsType =
        QString::fromLatin1(charging::protocol::request_type::kGetCoupons);
    const QString getNotificationsType =
        QString::fromLatin1(charging::protocol::request_type::kGetNotifications);
    const QString checkInType =
        QString::fromLatin1(charging::protocol::request_type::kCheckIn);
    const QString getPointsType =
        QString::fromLatin1(charging::protocol::request_type::kGetPoints);
    const QString submitChargerRatingType =
        QString::fromLatin1(charging::protocol::request_type::kSubmitChargerRating);
    const QString getMyRatingsType =
        QString::fromLatin1(charging::protocol::request_type::kGetMyRatings);

    if (type == getUserInfoType) {
        QJsonObject payload;
        payload.insert(QStringLiteral("user"), charging::model::toJson(user_));
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    if (type == updateUserInfoType) {
        // 契约 v1 §3 已冻结：公共校验（至少一项、nickname trim 1..32、
        // avatarKey [A-Za-z0-9_-]{0,64}、非法输入 INVALID_ARGUMENT + details.field）
        // 直接复用服务端正源 normalizeRequestData，mock 不再自写规则；
        // 清单成员检查按契约留给"Service"，mock 在此镜像服务端行为。
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const QJsonValue avatarKeyValue = normalized.value(QStringLiteral("avatarKey"));
        if (avatarKeyValue.isString() && !isBuiltInAvatarKey(avatarKeyValue.toString())) {
            charging::protocol::ProtocolError error =
                mockError(QString::fromLatin1(charging::protocol::error_code::kInvalidArgument),
                          QStringLiteral("Invalid user API request: avatarKey"));
            error.details.insert(QStringLiteral("field"), QStringLiteral("avatarKey"));
            callback(false, QJsonObject{}, error);
            return;
        }
        if (avatarKeyValue.isString()) {
            user_.avatarKey = avatarKeyValue.toString(); // "" = 默认首字头像
        }
        if (normalized.contains(QStringLiteral("nickname"))) {
            user_.nickname = normalized.value(QStringLiteral("nickname")).toString();
        }
        user_.updatedAtUtc = QDateTime::currentDateTimeUtc();
        QJsonObject payload;
        payload.insert(QStringLiteral("user"), charging::model::toJson(user_));
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    if (type == rechargeType) {
        // 契约 v1 §3（RECHARGE）已冻结：amountCents ∈ [1, 100000 元]、
        // transactionNo 必填且 [A-Za-z0-9_-]{1,40}，非法输入 INVALID_ARGUMENT +
        // details.field；公共校验复用服务端正源 normalizeRequestData。
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const qint64 amountCents = static_cast<qint64>(
            normalized.value(QStringLiteral("amountCents")).toDouble());
        const QString transactionNo =
            normalized.value(QStringLiteral("transactionNo")).toString();

        for (const auto& prior : records_) {
            if (prior.transactionNo != transactionNo) {
                continue;
            }
            if (prior.amountCents != amountCents) {
                // 同流水号不同金额：冲突，不泄漏原记录。
                callback(false, QJsonObject{},
                         mockError(QString::fromLatin1(
                                       charging::protocol::error_code::kIdempotencyConflict),
                                   QStringLiteral("transaction mismatch")));
                return;
            }
            if (prior.status != charging::model::RechargeStatus::Success) {
                // 同用户同金额但旧流水 FAILED：明确失败，不入账也不转成功。
                callback(false, QJsonObject{},
                         mockError(QString::fromLatin1(
                                       charging::protocol::error_code::kRechargeFailed),
                                   QStringLiteral("prior recharge failed")));
                return;
            }
            // 成功重放：返回原入账记录 + 当前余额（两者可不同，§2 两种余额语义）。
            callback(true, {{QStringLiteral("record"), charging::model::toJson(prior)},
                            {QStringLiteral("balanceCents"), user_.balanceCents},
                            {QStringLiteral("idempotent"), true}},
                     charging::protocol::ProtocolError{});
            return;
        }

        user_.balanceCents += amountCents;
        user_.updatedAtUtc = QDateTime::currentDateTimeUtc();
        grantRechargeCoupon(amountCents, user_.updatedAtUtc);

        charging::model::RechargeRecord record;
        record.id = nextRecordId_++;
        record.transactionNo = transactionNo;
        record.userId = user_.id;
        record.amountCents = amountCents;
        record.balanceAfterCents = user_.balanceCents;
        record.status = charging::model::RechargeStatus::Success;
        record.createdAtUtc = QDateTime::currentDateTimeUtc();
        records_.prepend(record);

        QJsonObject payload;
        payload.insert(QStringLiteral("record"), charging::model::toJson(record));
        payload.insert(QStringLiteral("balanceCents"), user_.balanceCents);
        payload.insert(QStringLiteral("idempotent"), false);
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    if (type == getRecordsType) {
        // 契约 v1 §3：分页参数校验（page ≥1、pageSize 1..100、默认 20）复用
        // normalizeRequestData；越界返回 INVALID_ARGUMENT，不静默修正。
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const int page = normalized.value(QStringLiteral("page")).toInt();
        const int pageSize = normalized.value(QStringLiteral("pageSize")).toInt();
        const int start = (page - 1) * pageSize;

        QJsonArray array;
        for (int index = start; index < records_.size() && index < start + pageSize; ++index) {
            array.append(charging::model::toJson(records_.at(index)));
        }

        QJsonObject payload;
        payload.insert(QStringLiteral("records"), array);
        payload.insert(QStringLiteral("page"), page);
        payload.insert(QStringLiteral("pageSize"), pageSize);
        payload.insert(QStringLiteral("total"), records_.size());
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    if (type == getUserStatsType) {
        // 与服务端 GET_USER_STATS 同口径：仅 COMPLETED、按月倒序、月数默认 6；
        // co2 = energyWh × 0.5568 g/Wh（全国电网平均排放因子，TODO(contract)）。
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const int months = normalized.value(QStringLiteral("months")).toInt(6);
        // 批次B：聚合档透传（normalize 已注入 "month" 缺省）。周档与服务端
        // strftime('%Y-W%W') 同语义：周一为始的年度周序号（tm 公式，00-53），
        // 不用 ISO 周（%G-%V 需 SQLite 3.44+）。字典序即时间序。
        const QString period = normalized.value(QStringLiteral("period")).toString(
            QStringLiteral("month"));
        auto periodKey = [&period](const QDateTime& utc) {
            const QDate date = utc.date();
            if (period == QLatin1String("year")) {
                return utc.toString(QStringLiteral("yyyy"));
            }
            if (period == QLatin1String("week")) {
                const int wday = date.dayOfWeek() % 7;     // Qt Mon=1..Sun=7 → tm Sun=0
                const int week = (date.dayOfYear() - 1 + 7 - wday) / 7;
                return QStringLiteral("%1-W%2").arg(date.year()).arg(week, 2, 10,
                                                                     QLatin1Char('0'));
            }
            return utc.toString(QStringLiteral("yyyy-MM"));
        };
        struct MonthAggregate
        {
            int orderCount = 0;
            qint64 energyWh = 0;
            qint64 amountCents = 0;
            qint64 durationSeconds = 0;
        };
        QHash<QString, MonthAggregate> byMonth;
        QStringList monthKeys;   // insertion order; orders_ is newest first
        for (const charging::model::Order& order : orders_) {
            if (order.status != charging::model::OrderStatus::Completed) {
                continue;
            }
            const QString key = periodKey(order.createdAtUtc.toUTC());
            if (!byMonth.contains(key)) {
                monthKeys.append(key);
            }
            MonthAggregate& aggregate = byMonth[key];
            ++aggregate.orderCount;
            aggregate.energyWh += order.energyWh;
            aggregate.amountCents += order.amountCents;
            aggregate.durationSeconds += order.durationSeconds;
        }
        std::sort(monthKeys.begin(), monthKeys.end(),
                  [](const QString& left, const QString& right) { return left > right; });
        QJsonArray array;
        for (int index = 0; index < monthKeys.size() && index < months; ++index) {
            const MonthAggregate& aggregate = byMonth.constFind(monthKeys.at(index)).value();
            QJsonObject item;
            item.insert(QStringLiteral("monthKey"), monthKeys.at(index));
            item.insert(QStringLiteral("orderCount"), aggregate.orderCount);
            item.insert(QStringLiteral("energyWh"), static_cast<double>(aggregate.energyWh));
            item.insert(QStringLiteral("amountCents"),
                        static_cast<double>(aggregate.amountCents));
            item.insert(QStringLiteral("durationSeconds"),
                        static_cast<double>(aggregate.durationSeconds));
            item.insert(QStringLiteral("co2Grams"), qRound64(aggregate.energyWh * 0.5568));
            array.append(item);
        }
        callback(true, QJsonObject{{QStringLiteral("months"), array}},
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == getCouponsType) {
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const QString statusFilter = normalized.value(QStringLiteral("status")).toString();
        // 审查 P2#4：与服务端同款派生状态口径——USED 最优先，其次按到期时间，
        // 存储 status 只当种子值；过滤、total、响应字段都只认有效态。
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QJsonArray matched;
        for (QJsonObject coupon : coupons_) {
            const QString stored = coupon.value(QStringLiteral("status")).toString();
            const qint64 expiresMs =
                qRound64(coupon.value(QStringLiteral("expiresAtUtc")).toDouble());
            const QString effective = stored == QLatin1String("used")
                ? stored
                : (expiresMs <= nowMs ? QStringLiteral("expired")
                                      : QStringLiteral("available"));
            coupon.insert(QStringLiteral("status"), effective);
            if (statusFilter.isEmpty() || effective == statusFilter) {
                matched.append(coupon);
            }
        }
        const int page = normalized.value(QStringLiteral("page")).toInt();
        const int pageSize = normalized.value(QStringLiteral("pageSize")).toInt();
        const int start = (page - 1) * pageSize;
        QJsonArray array;
        for (int index = start; index < matched.size() && index < start + pageSize; ++index) {
            array.append(matched.at(index));
        }
        callback(true, QJsonObject{{QStringLiteral("coupons"), array},
                                   {QStringLiteral("page"), page},
                                   {QStringLiteral("pageSize"), pageSize},
                                   {QStringLiteral("total"), matched.size()}},
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == getNotificationsType) {
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const int page = normalized.value(QStringLiteral("page")).toInt();
        const int pageSize = normalized.value(QStringLiteral("pageSize")).toInt();
        const int start = (page - 1) * pageSize;
        QJsonArray array;
        for (int index = start; index < notifications_.size() && index < start + pageSize;
             ++index) {
            const MockNotification& notification = notifications_.at(index);
            array.append(QJsonObject{
                {QStringLiteral("type"), notification.type.toLower()},
                {QStringLiteral("title"), notification.title},
                {QStringLiteral("body"), notification.body},
                {QStringLiteral("createdAtUtc"),
                 notification.createdAtUtc.toUTC().toString(Qt::ISODateWithMs)},
            });
        }
        callback(true, QJsonObject{{QStringLiteral("notifications"), array},
                                   {QStringLiteral("page"), page},
                                   {QStringLiteral("pageSize"), pageSize},
                                   {QStringLiteral("total"), notifications_.size()}},
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == checkInType) {
        // 批次C：日粒度幂等——镜像服务端 user_checkins (user_id, day) 主键；
        // 重放返回现总分 + gained=0，不报错。reason 存显示词（服务端对
        // CHECK_IN 映射"每日签到"，输出侧同款）。
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const QString day = QDateTime::currentDateTimeUtc().toUTC()
                                .toString(QStringLiteral("yyyy-MM-dd"));
        bool already = false;
        qint64 gained = 0;
        if (checkInDay_ == day) {
            already = true;
        } else {
            checkInDay_ = day;
            gained = charging::protocol::user_api::kCheckInRewardPoints;
            pointsTotal_ += gained;
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), QString::number(nextLedgerId_++));
            entry.insert(QStringLiteral("amount"), static_cast<double>(gained));
            entry.insert(QStringLiteral("reason"), QStringLiteral("每日签到"));
            entry.insert(QStringLiteral("createdAtUtc"),
                         QDateTime::currentDateTimeUtc().toUTC().toString(Qt::ISODateWithMs));
            pointsLedger_.prepend(entry);
        }
        callback(true, QJsonObject{{QStringLiteral("day"), day},
                                   {QStringLiteral("points"), static_cast<double>(pointsTotal_)},
                                   {QStringLiteral("gained"), static_cast<double>(gained)},
                                   {QStringLiteral("alreadyCheckedIn"), already}},
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == getPointsType) {
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const int page = normalized.value(QStringLiteral("page")).toInt();
        const int pageSize = normalized.value(QStringLiteral("pageSize")).toInt();
        const int start = (page - 1) * pageSize;
        QJsonArray array;
        for (int index = start; index < pointsLedger_.size() && index < start + pageSize;
             ++index) {
            array.append(pointsLedger_.at(index));
        }
        callback(true, QJsonObject{{QStringLiteral("points"), static_cast<double>(pointsTotal_)},
                                   {QStringLiteral("entries"), array},
                                   {QStringLiteral("page"), page},
                                   {QStringLiteral("pageSize"), pageSize},
                                   {QStringLiteral("total"), pointsLedger_.size()}},
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == submitChargerRatingType) {
        // 批次E：镜像服务端一单一评——订单必须存在且已完成，否则 NOT_FOUND；
        // order_id UNIQUE 幂等（重放不覆盖首评、不报错，TODO(contract) 改评二期）。
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const QString orderId = normalized.value(QStringLiteral("orderId")).toString();
        const charging::model::Order* order = nullptr;
        for (const charging::model::Order& candidate : orders_) {
            if (QString::number(candidate.id) == orderId) {
                order = &candidate;
                break;
            }
        }
        if (order == nullptr || order->status != charging::model::OrderStatus::Completed) {
            callback(false, QJsonObject{},
                     mockError(QString::fromLatin1(charging::protocol::error_code::kNotFound),
                               QStringLiteral("订单不存在或不可评价")));
            return;
        }
        QJsonObject existing;
        bool alreadyRated = false;
        for (const QJsonObject& candidate : ratings_) {
            if (candidate.value(QStringLiteral("orderId")).toString() == orderId) {
                existing = candidate;
                alreadyRated = true;
                break;
            }
        }
        if (!alreadyRated) {
            existing.insert(QStringLiteral("id"), QString::number(nextRatingId_++));
            existing.insert(QStringLiteral("orderId"), orderId);
            existing.insert(QStringLiteral("chargerId"), QString::number(order->chargerId));
            const auto display = chargerDisplays_.constFind(order->chargerId);
            if (display != chargerDisplays_.constEnd()) {
                existing.insert(QStringLiteral("stationName"), display->first);
                existing.insert(QStringLiteral("chargerCode"), display->second);
            }
            existing.insert(QStringLiteral("rating"), normalized.value(QStringLiteral("rating")));
            existing.insert(QStringLiteral("comment"),
                            normalized.value(QStringLiteral("comment")));
            existing.insert(QStringLiteral("createdAtUtc"),
                            QDateTime::currentDateTimeUtc().toUTC().toString(Qt::ISODateWithMs));
            ratings_.prepend(existing);
        }
        callback(true, QJsonObject{{QStringLiteral("rating"), existing},
                                   {QStringLiteral("alreadyRated"), alreadyRated}},
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == getMyRatingsType) {
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const int page = normalized.value(QStringLiteral("page")).toInt();
        const int pageSize = normalized.value(QStringLiteral("pageSize")).toInt();
        const int start = (page - 1) * pageSize;
        QJsonArray array;
        for (int index = start; index < ratings_.size() && index < start + pageSize; ++index) {
            array.append(ratings_.at(index));
        }
        callback(true, QJsonObject{{QStringLiteral("ratings"), array},
                                   {QStringLiteral("page"), page},
                                   {QStringLiteral("pageSize"), pageSize},
                                   {QStringLiteral("total"), ratings_.size()}},
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == getOrdersType) {
        // 契约 v1 §3：status 枚举与分页参数校验复用 normalizeRequestData；
        // 排序冻结为 createdAt DESC, id DESC，mock 显式排序而非依赖存储顺序。
        QJsonObject normalized;
        charging::protocol::ProtocolError contractError;
        if (!charging::protocol::user_api::normalizeRequestData(type, data, &normalized,
                                                                &contractError)) {
            callback(false, QJsonObject{}, contractError);
            return;
        }
        const QString statusFilter = normalized.value(QStringLiteral("status")).toString();
        QVector<const charging::model::Order*> matched;
        for (const charging::model::Order& order : orders_) {
            if (statusFilter.isEmpty() ||
                charging::model::toString(order.status) == statusFilter) {
                matched.append(&order);
            }
        }
        std::sort(matched.begin(), matched.end(),
                  [](const charging::model::Order* a, const charging::model::Order* b) {
                      if (a->createdAtUtc != b->createdAtUtc) {
                          return a->createdAtUtc > b->createdAtUtc;
                      }
                      return a->id > b->id;
                  });

        const int page = normalized.value(QStringLiteral("page")).toInt();
        const int pageSize = normalized.value(QStringLiteral("pageSize")).toInt();
        const int start = (page - 1) * pageSize;

        QJsonArray array;
        for (int index = start; index < matched.size() && index < start + pageSize; ++index) {
            const charging::model::Order& order = *matched.at(index);
            QJsonObject object = charging::model::toJson(order);
            // 契约 v1 已冻结 OrderSummary = 平铺完整 Order + 必填
            // stationName/chargerCode；未知映射回退空串也必须携带字段。
            const auto display = chargerDisplays_.constFind(order.chargerId);
            object.insert(QStringLiteral("stationName"),
                          display != chargerDisplays_.constEnd() ? display->first
                                                                 : QStringLiteral(""));
            object.insert(QStringLiteral("chargerCode"),
                          display != chargerDisplays_.constEnd() ? display->second
                                                                 : QStringLiteral(""));
            array.append(object);
        }

        QJsonObject payload;
        payload.insert(QStringLiteral("orders"), array);
        payload.insert(QStringLiteral("page"), page);
        payload.insert(QStringLiteral("pageSize"), pageSize);
        payload.insert(QStringLiteral("total"), matched.size());
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    if (type == getChargingStatusType) {
        const qint64 orderId =
            data.value(QStringLiteral("orderId")).toString().toLongLong();
        charging::model::Order* order = findOrder(orderId);
        if (order == nullptr) {
            callback(false, QJsonObject{},
                     mockError(QString::fromLatin1(charging::protocol::error_code::kNotFound),
                               QStringLiteral("order not found")));
            return;
        }

        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (order->status == charging::model::OrderStatus::Charging) {
            // §8.4: a live derived snapshot; the read never writes back.
            const qint64 liveDuration =
                order->startedAtUtc.isValid() ? order->startedAtUtc.secsTo(now) : 0;
            const qint64 liveEnergy = liveDuration * kMockLivePowerWatts / 3600; // Wh
            callback(true,
                     buildStatusPayload(*order, kMockLivePowerWatts, liveEnergy, liveDuration),
                     charging::protocol::ProtocolError{});
        } else {
            // §8.4: non-charging orders return the persisted row with
            // currentPowerWatts = 0 — still a success, not an error.
            callback(true,
                     buildStatusPayload(*order, 0, order->energyWh, order->durationSeconds),
                     charging::protocol::ProtocolError{});
        }
        return;
    }

    if (type == stopChargingType) {
        const qint64 orderId =
            data.value(QStringLiteral("orderId")).toString().toLongLong();
        charging::model::Order* order = findOrder(orderId);
        if (order == nullptr) {
            callback(false, QJsonObject{},
                     mockError(QString::fromLatin1(charging::protocol::error_code::kNotFound),
                               QStringLiteral("order not found")));
            return;
        }
        if (order->status == charging::model::OrderStatus::WaitingPayment ||
            order->status == charging::model::OrderStatus::Completed) {
            // §8.5: a retry after a successful stop is an idempotent success
            // replay of the saved result (no double billing).
            callback(true,
                     buildStatusPayload(*order, 0, order->energyWh, order->durationSeconds),
                     charging::protocol::ProtocolError{});
            return;
        }
        if (order->status != charging::model::OrderStatus::Charging) {
            callback(false, QJsonObject{},
                     mockError(
                         QString::fromLatin1(
                             charging::protocol::error_code::kInvalidStateTransition),
                         QStringLiteral("order is not charging")));
            return;
        }

        // The mock plays the server's billing role: settle with the live
        // snapshot and move the order to WAITING_PAYMENT. The real
        // transition is decided by the leader's ChargingService.
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const qint64 liveDuration =
            order->startedAtUtc.isValid() ? order->startedAtUtc.secsTo(now) : 0;
        const qint64 liveEnergy = liveDuration * kMockLivePowerWatts / 3600; // Wh
        order->durationSeconds = liveDuration;
        order->energyWh = liveEnergy;
        order->amountCents = amountFor(liveEnergy, order->unitPriceCentsPerKwh);
        order->stoppedAtUtc = now;
        order->status = charging::model::OrderStatus::WaitingPayment;
        order->updatedAtUtc = now;
        // 镜像服务端 STOP 事务里的通知落库（文案同款，TODO(contract) 评审）。
        appendMockNotification(QStringLiteral("CHARGING_STOPPED"),
                               QStringLiteral("充电已结束"),
                               QStringLiteral("本次充电 %1 kWh，用时 %2 分钟，产生费用 ¥%3，请及时支付")
                                   .arg(liveEnergy / 1000.0, 0, 'f', 2)
                                   .arg(liveDuration / 60)
                                   .arg(order->amountCents / 100.0, 0, 'f', 2),
                               now);

        callback(true,
                 buildStatusPayload(*order, 0, order->energyWh, order->durationSeconds),
                 charging::protocol::ProtocolError{});
        return;
    }

    if (type == payOrderType) {
        const qint64 orderId =
            data.value(QStringLiteral("orderId")).toString().toLongLong();
        charging::model::Order* order = findOrder(orderId);
        if (order == nullptr) {
            callback(false, QJsonObject{},
                     mockError(QString::fromLatin1(charging::protocol::error_code::kNotFound),
                               QStringLiteral("order not found")));
            return;
        }
        if (order->status == charging::model::OrderStatus::Completed) {
            // §8.6: retrying payment on an already-completed order is an
            // idempotent success — return the saved order + current balance
            // and charge nothing (mirrors the server's no-double-debit rule).
            callback(true, payResultPayload(*order), charging::protocol::ProtocolError{});
            return;
        }
        if (order->status != charging::model::OrderStatus::WaitingPayment) {
            callback(false, QJsonObject{},
                     mockError(
                         QString::fromLatin1(
                             charging::protocol::error_code::kInvalidStateTransition),
                         QStringLiteral("order is not awaiting payment")));
            return;
        }
        if (user_.balanceCents < order->amountCents) {
            QJsonObject details;
            details.insert(QStringLiteral("balanceCents"), user_.balanceCents);
            details.insert(QStringLiteral("amountCents"), order->amountCents);
            charging::protocol::ProtocolError error = mockError(
                QString::fromLatin1(charging::protocol::error_code::kInsufficientBalance),
                QStringLiteral("insufficient balance"));
            error.details = details;
            callback(false, QJsonObject{}, error);
            return;
        }

        // TODO(contract): whether PAY_ORDER also appends to the recharge/
        // transaction ledger is the leader/BillingService's decision; the mock
        // only moves balance and order state.
        const QDateTime now = QDateTime::currentDateTimeUtc();
        user_.balanceCents -= order->amountCents;
        user_.updatedAtUtc = now;
        order->status = charging::model::OrderStatus::Completed;
        order->paidAtUtc = now;
        order->updatedAtUtc = now;
        // 镜像服务端 PAY 事务里的通知落库（幂等重放分支已在上方返回）。
        appendMockNotification(QStringLiteral("ORDER_PAID"),
                               QStringLiteral("支付成功"),
                               QStringLiteral("订单 %1 已支付 ¥%2，当前余额 ¥%3")
                                   .arg(order->orderNo)
                                   .arg(order->amountCents / 100.0, 0, 'f', 2)
                                   .arg(user_.balanceCents / 100.0, 0, 'f', 2),
                               now);
        // 镜像服务端同事务的结算返积分（2026-09-09）：规则单点 =
        // settlementRewardPoints()（每满 1 元返 1 分，TODO(contract)）。
        // reason 存显示词与 CHECK_IN 同款（真服务端存码、输出侧映射）。
        const qint64 rewardPoints =
            charging::protocol::user_api::settlementRewardPoints(order->amountCents);
        if (rewardPoints > 0) {
            pointsTotal_ += rewardPoints;
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), QString::number(nextLedgerId_++));
            entry.insert(QStringLiteral("amount"), static_cast<double>(rewardPoints));
            entry.insert(QStringLiteral("reason"), QStringLiteral("消费返积分"));
            entry.insert(QStringLiteral("createdAtUtc"), now.toString(Qt::ISODateWithMs));
            pointsLedger_.prepend(entry);
        }

        callback(true, payResultPayload(*order), charging::protocol::ProtocolError{});
        return;
    }

    if (type == startChargingType) {
        // Server shape: {"reservationId": "<decimal string>"}; the mock also
        // accepts 0 = walk-up "scan" start, which the server does not define
        // yet (TODO(contract)) — demo-only.
        const qint64 reservationId =
            data.value(QStringLiteral("reservationId")).toString().toLongLong();
        for (const charging::model::Order& existing : orders_) {
            if (existing.status == charging::model::OrderStatus::Charging) {
                callback(false, QJsonObject{},
                         mockError(QString::fromLatin1(
                                       charging::protocol::error_code::kInvalidStateTransition),
                                   QStringLiteral("an order is already charging")));
                return;
            }
        }

        qint64 chargerId = 0;
        if (reservationId <= 0) {
            chargerId = 22; // the demo walk-up charger.
            chargerDisplays_.insert(chargerId,
                                    {QStringLiteral("云杉科技园区充电站"), QStringLiteral("A05")});
        } else {
            const auto known = mockReservations_.constFind(reservationId);
            if (known == mockReservations_.constEnd()) {
                callback(false, QJsonObject{},
                         mockError(QString::fromLatin1(charging::protocol::error_code::kNotFound),
                                   QStringLiteral("reservation not found")));
                return;
            }
            chargerId = known->first;
            chargerDisplays_.insert(chargerId, known->second);
        }

        const QDateTime now = QDateTime::currentDateTimeUtc();
        charging::model::Order order;
        order.id = nextOrderId_++;
        order.orderNo = QStringLiteral("MOCKORD%1%2")
                            .arg(now.toUTC().toString(QStringLiteral("yyyyMMdd")),
                                 QString::number(order.id));
        order.userId = user_.id;
        order.chargerId = chargerId;
        order.reservationId = reservationId > 0 ? reservationId : 0;
        order.status = charging::model::OrderStatus::Charging;
        order.unitPriceCentsPerKwh = 150;
        order.createdAtUtc = now;
        order.startedAtUtc = now;
        order.updatedAtUtc = now;
        orders_.prepend(order); // keep newest first

        callback(true, buildStatusPayload(order, kMockLivePowerWatts, 0, 0),
                 charging::protocol::ProtocolError{});
        return;
    }

    callback(false, QJsonObject{},
             mockError(QString::fromLatin1(charging::protocol::error_code::kUnknownRequestType),
                       QStringLiteral("mock transport does not implement: ") + type));
}

} // namespace charging::client
