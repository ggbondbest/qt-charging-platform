#include "request_dispatcher.h"

#include "charging/common/model/model_json.h"
#include "charging/common/protocol/protocol.h"
#include "charging_service.h"
#include "order_service.h"
#include "user_service.h"

#include <QJsonObject>
#include <QJsonValue>

namespace charging::server {

namespace {

charging::protocol::ProtocolError unauthorizedError()
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kUnauthorized);
    error.message = QStringLiteral("请先登录后再执行此操作");
    return error;
}

charging::protocol::ProtocolError invalidIdError(const QString& fieldName)
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope);
    error.message = QStringLiteral("'%1' 必须是正十进制 ID 字符串").arg(fieldName);
    error.details.insert(QStringLiteral("field"), fieldName);
    return error;
}

charging::protocol::ProtocolError serviceUnavailableError()
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kInternalError);
    error.message = QStringLiteral("服务端业务服务尚未就绪");
    return error;
}

bool readPositiveId(const QJsonObject& data, const char* key, qint64* outValue,
                    charging::protocol::ProtocolError* error)
{
    const QString fieldName = QString::fromLatin1(key);
    const QJsonValue value = data.value(fieldName);
    if (!value.isString()) {
        if (error != nullptr) {
            *error = invalidIdError(fieldName);
        }
        return false;
    }

    const QString text = value.toString();
    if (text.isEmpty() || text.front() < QLatin1Char('1') || text.front() > QLatin1Char('9')) {
        if (error != nullptr) {
            *error = invalidIdError(fieldName);
        }
        return false;
    }
    for (const QChar character : text) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
            if (error != nullptr) {
                *error = invalidIdError(fieldName);
            }
            return false;
        }
    }

    bool conversionSucceeded = false;
    const qint64 id = text.toLongLong(&conversionSucceeded, 10);
    if (!conversionSucceeded || id <= 0) {
        if (error != nullptr) {
            *error = invalidIdError(fieldName);
        }
        return false;
    }
    *outValue = id;
    return true;
}

bool isChargingAction(const QString& type)
{
    using namespace charging::protocol::request_type;
    return type == QString::fromLatin1(kReserveCharger) ||
           type == QString::fromLatin1(kCancelReservation) ||
           type == QString::fromLatin1(kStartCharging) ||
           type == QString::fromLatin1(kGetChargingStatus) ||
           type == QString::fromLatin1(kStopCharging);
}

} // namespace

RequestDispatcher::RequestDispatcher(UserService* userService, ChargingService* chargingService,
                                     OrderService* orderService)
    : userService_(userService), chargingService_(chargingService), orderService_(orderService)
{
    Q_ASSERT(userService_ != nullptr);
}

charging::protocol::ResponseEnvelope
RequestDispatcher::dispatch(const charging::protocol::RequestEnvelope& request,
                            qint64* authenticatedUserId) const
{
    using namespace charging::protocol::request_type;

    if (request.type == QString::fromLatin1(kUserLogin)) {
        const QJsonValue phoneValue = request.data.value(QStringLiteral("phone"));
        const QString phone = phoneValue.isString() ? phoneValue.toString() : QString();
        const LoginResult login = userService_->loginOrRegister(phone);
        if (!login.success) {
            return charging::protocol::makeErrorResponse(request, login.error);
        }

        if (authenticatedUserId != nullptr) {
            *authenticatedUserId = login.user.id;
        }
        QJsonObject responseData;
        responseData.insert(QStringLiteral("created"), login.created);
        responseData.insert(QStringLiteral("user"), charging::model::toJson(login.user));
        return charging::protocol::makeSuccessResponse(request, responseData);
    }

    const bool payAction = request.type == QString::fromLatin1(kPayOrder);
    if (isChargingAction(request.type) || payAction) {
        if (authenticatedUserId == nullptr || *authenticatedUserId <= 0) {
            return charging::protocol::makeErrorResponse(request, unauthorizedError());
        }

        const bool usesChargerId = request.type == QString::fromLatin1(kReserveCharger);
        const bool usesReservationId = request.type == QString::fromLatin1(kCancelReservation) ||
                                       request.type == QString::fromLatin1(kStartCharging);
        const char* idKey =
            usesChargerId ? "chargerId" : (usesReservationId ? "reservationId" : "orderId");
        qint64 targetId = 0;
        charging::protocol::ProtocolError idError;
        if (!readPositiveId(request.data, idKey, &targetId, &idError)) {
            return charging::protocol::makeErrorResponse(request, idError);
        }

        if (payAction) {
            if (orderService_ == nullptr) {
                return charging::protocol::makeErrorResponse(request, serviceUnavailableError());
            }
            const PaymentResult result = orderService_->pay(*authenticatedUserId, targetId);
            if (!result.success) {
                return charging::protocol::makeErrorResponse(request, result.error);
            }
            QJsonObject responseData;
            responseData.insert(QStringLiteral("order"), charging::model::toJson(result.order));
            responseData.insert(QStringLiteral("balanceCents"),
                                static_cast<double>(result.balanceCents));
            return charging::protocol::makeSuccessResponse(request, responseData);
        }

        if (chargingService_ == nullptr) {
            return charging::protocol::makeErrorResponse(request, serviceUnavailableError());
        }

        ChargingOperationResult result;
        if (request.type == QString::fromLatin1(kReserveCharger)) {
            result = chargingService_->reserve(*authenticatedUserId, targetId);
        } else if (request.type == QString::fromLatin1(kCancelReservation)) {
            result = chargingService_->cancelReservation(*authenticatedUserId, targetId);
        } else if (request.type == QString::fromLatin1(kStartCharging)) {
            result = chargingService_->startCharging(*authenticatedUserId, targetId);
        } else if (request.type == QString::fromLatin1(kGetChargingStatus)) {
            result = chargingService_->chargingStatus(*authenticatedUserId, targetId);
        } else {
            result = chargingService_->stopCharging(*authenticatedUserId, targetId);
        }

        if (!result.success) {
            return charging::protocol::makeErrorResponse(request, result.error);
        }

        QJsonObject responseData;
        responseData.insert(QStringLiteral("order"), charging::model::toJson(result.order));
        if (request.type == QString::fromLatin1(kReserveCharger) ||
            request.type == QString::fromLatin1(kCancelReservation) ||
            request.type == QString::fromLatin1(kStartCharging)) {
            responseData.insert(QStringLiteral("reservation"),
                                charging::model::toJson(result.reservation));
        } else if (request.type == QString::fromLatin1(kGetChargingStatus)) {
            responseData.insert(QStringLiteral("currentPowerWatts"), result.currentPowerWatts);
        }
        return charging::protocol::makeSuccessResponse(request, responseData);
    }

    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kUnknownRequestType);
    error.message = QStringLiteral("服务端尚未实现请求类型：%1").arg(request.type);
    error.details.insert(QStringLiteral("type"), request.type);
    return charging::protocol::makeErrorResponse(request, error);
}

} // namespace charging::server
