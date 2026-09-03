#include "charging/client/profile_charging/mock_request_transport.h"

#include "charging/common/model/enums.h"
#include "charging/common/model/model_json.h"
#include "charging/common/protocol/protocol.h"

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
    const QVector<SeedRecord> seeds = {
        {3000, 10000, QStringLiteral("2026-08-28T09:12:00.000Z"),
         charging::model::RechargeStatus::Success},
        {5000, 7000, QStringLiteral("2026-08-20T12:41:00.000Z"),
         charging::model::RechargeStatus::Success},
        {2000, 2000, QStringLiteral("2026-08-05T07:55:00.000Z"),
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
    orders_.append(makeOrder(12, charging::model::OrderStatus::Cancelled, 98, 0, 0,
                             0, 0, 0));
    orders_[orders_.size() - 1].createdAtUtc = now.addDays(-7);
    orders_[orders_.size() - 1].updatedAtUtc = now.addDays(-7);
    orders_[orders_.size() - 1].amountCents = 0;

    orders_.append(makeOrder(11, charging::model::OrderStatus::Completed, 132, 24680, 5538,
                             3 * 24 * 3600, 3 * 24 * 3600 - 5538, 3 * 24 * 3600 - 5538 - 60));
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
    // TODO(contract): GET_CHARGING_STATUS / STOP_CHARGING payload shape is not
    // frozen in docs/api/socket_protocol.md yet. This mirrors the expected
    // order object plus live keys powerWatts/estimatedAmountCents and the
    // stationName/chargerCode join. Confirm with the leader before wiring.
    QJsonObject object = charging::model::toJson(order);
    object.insert(QStringLiteral("energyWh"), energyWh);
    object.insert(QStringLiteral("durationSeconds"), durationSeconds);
    const auto display = chargerDisplays_.constFind(order.chargerId);
    if (display != chargerDisplays_.constEnd()) {
        object.insert(QStringLiteral("stationName"), display->first);
        object.insert(QStringLiteral("chargerCode"), display->second);
    }
    object.insert(QStringLiteral("powerWatts"), powerWatts);
    object.insert(QStringLiteral("estimatedAmountCents"),
                  amountFor(energyWh, order.unitPriceCentsPerKwh));

    QJsonObject payload;
    payload.insert(QStringLiteral("status"), object);
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
    const QString payOrderType =
        QString::fromLatin1(charging::protocol::request_type::kPayOrder);

    if (type == getUserInfoType) {
        QJsonObject payload;
        payload.insert(QStringLiteral("user"), charging::model::toJson(user_));
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    if (type == rechargeType) {
        const qint64 amountCents = static_cast<qint64>(
            data.value(QStringLiteral("amountCents")).toDouble());
        if (amountCents < 1) {
            // TODO(contract): the validation error code for a bad recharge
            // amount is not frozen yet; INVALID_ENVELOPE is a stand-in.
            callback(false, QJsonObject{},
                     mockError(QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope),
                               QStringLiteral("amountCents must be a positive integer")));
            return;
        }
        user_.balanceCents += amountCents;
        user_.updatedAtUtc = QDateTime::currentDateTimeUtc();

        charging::model::RechargeRecord record;
        record.id = nextRecordId_++;
        record.transactionNo =
            QStringLiteral("MOCKRCH%1").arg(nextTransactionSeq_++, 8, 10, QChar('0'));
        record.userId = user_.id;
        record.amountCents = amountCents;
        record.balanceAfterCents = user_.balanceCents;
        record.status = charging::model::RechargeStatus::Success;
        record.createdAtUtc = QDateTime::currentDateTimeUtc();
        records_.prepend(record);

        QJsonObject payload;
        payload.insert(QStringLiteral("amountCents"), amountCents);
        payload.insert(QStringLiteral("balanceAfterCents"), user_.balanceCents);
        payload.insert(QStringLiteral("transactionNo"), record.transactionNo);
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    if (type == getRecordsType) {
        const int page = data.value(QStringLiteral("page")).toInt(1);
        const int pageSize = data.value(QStringLiteral("pageSize")).toInt(10);
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

    if (type == getOrdersType) {
        const QString statusFilter = data.value(QStringLiteral("status")).toString();
        QVector<const charging::model::Order*> matched;
        for (const charging::model::Order& order : orders_) {
            if (statusFilter.isEmpty() ||
                charging::model::toString(order.status) == statusFilter) {
                matched.append(&order);
            }
        }

        const int page = data.value(QStringLiteral("page")).toInt(1);
        const int pageSize = data.value(QStringLiteral("pageSize")).toInt(10);
        const int start = (page - 1) * pageSize;

        QJsonArray array;
        for (int index = start; index < matched.size() && index < start + pageSize; ++index) {
            const charging::model::Order& order = *matched.at(index);
            QJsonObject object = charging::model::toJson(order);
            const auto display = chargerDisplays_.constFind(order.chargerId);
            // TODO(contract): server-side join is not frozen yet; the mock
            // mirrors the expected stationName/chargerCode extra fields.
            if (display != chargerDisplays_.constEnd()) {
                object.insert(QStringLiteral("stationName"), display->first);
                object.insert(QStringLiteral("chargerCode"), display->second);
            }
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

    if (type == getChargingStatusType || type == stopChargingType) {
        // TODO(contract): request field name is assumed as orderId (decimal
        // string per the id convention); confirm with the leader.
        const qint64 orderId =
            data.value(QStringLiteral("orderId")).toString().toLongLong();
        charging::model::Order* order = findOrder(orderId);
        if (order == nullptr) {
            callback(false, QJsonObject{},
                     mockError(QString::fromLatin1(charging::protocol::error_code::kNotFound),
                               QStringLiteral("order not found")));
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

        const QDateTime now = QDateTime::currentDateTimeUtc();
        const qint64 liveDuration =
            order->startedAtUtc.isValid() ? order->startedAtUtc.secsTo(now) : 0;
        const qint64 liveEnergy = liveDuration * kMockLivePowerWatts / 3600; // Wh

        if (type == stopChargingType) {
            // The mock plays the server's billing role: settle with the live
            // snapshot and move the order to WAITING_PAYMENT. The real
            // transition is decided by the leader's ChargingService.
            order->durationSeconds = liveDuration;
            order->energyWh = liveEnergy;
            order->amountCents = amountFor(liveEnergy, order->unitPriceCentsPerKwh);
            order->stoppedAtUtc = now;
            order->status = charging::model::OrderStatus::WaitingPayment;
            order->updatedAtUtc = now;
        }

        callback(true,
                 buildStatusPayload(*order,
                                    type == stopChargingType ? 0 : kMockLivePowerWatts,
                                    liveEnergy, liveDuration),
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

        QJsonObject payload;
        payload.insert(QStringLiteral("amountCents"), order->amountCents);
        payload.insert(QStringLiteral("balanceAfterCents"), user_.balanceCents);
        payload.insert(QStringLiteral("paidAt"),
                       now.toString(Qt::ISODateWithMs)); // "2026-...T...Z"
        callback(true, payload, charging::protocol::ProtocolError{});
        return;
    }

    callback(false, QJsonObject{},
             mockError(QString::fromLatin1(charging::protocol::error_code::kUnknownRequestType),
                       QStringLiteral("mock transport does not implement: ") + type));
}

} // namespace charging::client
