#include "charging/client/profile_charging/charging_service.h"

#include "charging/common/model/model_json.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

namespace charging::client {

namespace {

charging::protocol::ProtocolError makeLocalError(const char* code, const QString& message)
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(code);
    error.message = message;
    return error;
}

qint64 jsonPositiveInt(const QJsonValue& value)
{
    // JSON numbers that must stay non-negative (cents, watts); clamp
    // instead of trusting them silently.
    const double number = value.toDouble(0.0);
    return number > 0.0 ? static_cast<qint64>(number) : 0;
}

} // namespace

ChargingService::ChargingService(IRequestTransport* transport, QObject* parent)
    : QObject(parent), transport_(transport)
{
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(kStatusPollIntervalMs);
    connect(pollTimer_, &QTimer::timeout, this, &ChargingService::handlePollTick);
}

ChargingService::~ChargingService()
{
    pollTimer_->stop();
}

bool ChargingService::isTracking() const
{
    return pollTimer_->isActive() && trackedOrderId_ > 0;
}

bool ChargingService::isStoppingCharging() const
{
    return stopping_;
}

bool ChargingService::isPaying() const
{
    return paying_;
}

void ChargingService::startTracking(qint64 orderId)
{
    if (orderId <= 0) {
        return;
    }
    trackedOrderId_ = orderId;
    pollTimer_->start();
    fetchStatusNow();
}

void ChargingService::stopTracking()
{
    pollTimer_->stop();
}

void ChargingService::handlePollTick()
{
    fetchStatusNow(); // guard inside handles the in-flight case
}

void ChargingService::fetchStatusNow()
{
    const QString type =
        QString::fromLatin1(charging::protocol::request_type::kGetChargingStatus);
    if (trackedOrderId_ <= 0 || fetchingStatus_) {
        return; // Nothing tracked, or previous poll still in flight: skip.
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }

    fetchingStatus_ = true;
    QJsonObject payload;
    // TODO(contract): request field name assumed as orderId (decimal string
    // per the id convention); confirm with the leader before wiring.
    payload.insert(QStringLiteral("orderId"), QString::number(trackedOrderId_));

    transport_->send(
        type, payload,
        [this, type](bool success, const QJsonObject& data,
                     const charging::protocol::ProtocolError& error) {
            fetchingStatus_ = false;
            if (!success) {
                emit operationFailed(type, error);
                return;
            }
            ChargingStatus status;
            QString parseError;
            if (!parseStatus(data.value(QStringLiteral("status")).toObject(), &status,
                             &parseError)) {
                emit operationFailed(type,
                                     makeLocalError(charging::protocol::error_code::kInternalError,
                                                    QStringLiteral("invalid status payload: ") +
                                                        parseError));
                return;
            }
            emit statusLoaded(status);
        });
}

void ChargingService::stopCharging()
{
    const QString type = QString::fromLatin1(charging::protocol::request_type::kStopCharging);
    if (stopping_ || trackedOrderId_ <= 0) {
        return; // Duplicate submission guard.
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }

    stopping_ = true;
    QJsonObject payload;
    payload.insert(QStringLiteral("orderId"), QString::number(trackedOrderId_));

    transport_->send(
        type, payload,
        [this, type](bool success, const QJsonObject& data,
                     const charging::protocol::ProtocolError& error) {
            stopping_ = false;
            if (!success) {
                emit operationFailed(type, error);
                return;
            }
            ChargingStatus status;
            QString parseError;
            if (!parseStatus(data.value(QStringLiteral("status")).toObject(), &status,
                             &parseError)) {
                emit operationFailed(type,
                                     makeLocalError(charging::protocol::error_code::kInternalError,
                                                    QStringLiteral("invalid status payload: ") +
                                                        parseError));
                return;
            }
            stopTracking();
            emit stopCompleted(status);
        });
}

void ChargingService::payOrder(qint64 orderId)
{
    const QString type = QString::fromLatin1(charging::protocol::request_type::kPayOrder);
    if (paying_) {
        return; // Duplicate submission guard.
    }
    if (orderId <= 0) {
        emit operationFailed(type,
                             makeLocalError(charging::protocol::error_code::kInvalidEnvelope,
                                            QStringLiteral("invalid order id")));
        return;
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }

    paying_ = true;
    QJsonObject payload;
    payload.insert(QStringLiteral("orderId"), QString::number(orderId));

    transport_->send(
        type, payload,
        [this, type](bool success, const QJsonObject& data,
                     const charging::protocol::ProtocolError& error) {
            paying_ = false;
            if (!success) {
                emit operationFailed(type, error);
                return;
            }
            // TODO(contract): PAY_ORDER response fields assumed; the real
            // settlement result is server-authoritative (BillingService).
            const qint64 amountCents = jsonPositiveInt(data.value(QStringLiteral("amountCents")));
            const qint64 balanceAfter =
                jsonPositiveInt(data.value(QStringLiteral("balanceAfterCents")));
            emit paymentCompleted(amountCents, balanceAfter);
        });
}

bool ChargingService::parseStatus(const QJsonObject& object, ChargingStatus* outStatus,
                                  QString* errorMessage)
{
    if (object.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("missing \"status\" object");
        }
        return false;
    }
    ChargingStatus status;
    if (!charging::model::fromJson(object, &status.order, errorMessage)) {
        return false;
    }
    status.stationName = object.value(QStringLiteral("stationName")).toString();
    status.chargerCode = object.value(QStringLiteral("chargerCode")).toString();
    // TODO(contract): live power field name (powerWatts vs currentPowerWatts)
    // is not frozen; absence degrades the UI to "-- kW", never a guess.
    status.powerKnown = object.contains(QStringLiteral("powerWatts"));
    status.powerWatts = jsonPositiveInt(object.value(QStringLiteral("powerWatts")));
    status.estimatedAmountCents =
        jsonPositiveInt(object.value(QStringLiteral("estimatedAmountCents")));
    *outStatus = status;
    return true;
}

} // namespace charging::client
