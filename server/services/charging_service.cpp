#include "charging_service.h"

#include "billing_service.h"
#include "charging_repository.h"
#include "charging_state_machine.h"
#include "workflow_repository_types.h"

#include <QDebug>
#include <QJsonObject>
#include <QUuid>

#include <utility>

namespace charging::server {

namespace {

charging::protocol::ProtocolError makeError(const char* code, const QString& message)
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(code);
    error.message = message;
    return error;
}

charging::protocol::ProtocolError mapRepositoryError(RepositoryError error,
                                                     const QString& diagnostic)
{
    Q_UNUSED(diagnostic);
    using namespace charging::protocol;
    switch (error) {
    case RepositoryError::InvalidInput:
        return makeError(error_code::kInvalidEnvelope, QStringLiteral("请求参数无效"));
    case RepositoryError::NotFound:
        return makeError(error_code::kNotFound, QStringLiteral("指定的业务资源不存在"));
    case RepositoryError::Unauthorized:
        return makeError(error_code::kUnauthorized, QStringLiteral("无权操作该业务资源"));
    case RepositoryError::UserFrozen:
        return makeError(error_code::kUserFrozen, QStringLiteral("该用户已被冻结"));
    case RepositoryError::ChargerNotAvailable:
        return makeError(error_code::kChargerNotAvailable, QStringLiteral("充电桩当前不可预约"));
    case RepositoryError::ExistingUnfinishedOrder:
        return makeError(error_code::kInvalidStateTransition,
                         QStringLiteral("当前用户已有未完成订单"));
    case RepositoryError::InvalidStateTransition:
        return makeError(error_code::kInvalidStateTransition,
                         QStringLiteral("当前状态不允许执行此操作"));
    case RepositoryError::InsufficientBalance:
        return makeError(error_code::kInsufficientBalance, QStringLiteral("账户余额不足"));
    case RepositoryError::ArithmeticOverflow:
        return makeError(error_code::kInternalError, QStringLiteral("充电计量结果超出系统范围"));
    case RepositoryError::Database:
        qWarning().noquote() << "Charging workflow database operation failed";
        return makeError(error_code::kDatabaseError, QStringLiteral("数据库操作失败，请稍后重试"));
    case RepositoryError::None:
        break;
    }
    return makeError(error_code::kInternalError, QStringLiteral("服务器内部状态异常"));
}

QString makeOrderNo(const QDateTime& nowUtc)
{
    const QString timestamp = nowUtc.toUTC().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
    const QString random = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12).toUpper();
    return QStringLiteral("ORD-%1-%2").arg(timestamp, random);
}

} // namespace

ChargingService::ChargingService(ChargingRepository* chargingRepository,
                                 BillingService* billingService, UtcClock clock)
    : chargingRepository_(chargingRepository), billingService_(billingService),
      clock_(std::move(clock))
{
    Q_ASSERT(chargingRepository_ != nullptr);
    Q_ASSERT(billingService_ != nullptr);
}

ChargingOperationResult ChargingService::reserve(qint64 userId, qint64 chargerId) const
{
    const QDateTime now = utcNow(clock_);
    const QDateTime expiresAt = now.addSecs(kReservationLifetimeSeconds);
    return fromRepository(
        chargingRepository_->reserve(userId, chargerId, now, expiresAt, makeOrderNo(now)));
}

ChargingOperationResult ChargingService::cancelReservation(qint64 userId,
                                                           qint64 reservationId) const
{
    return fromRepository(
        chargingRepository_->cancelReservation(userId, reservationId, utcNow(clock_)));
}

ChargingOperationResult ChargingService::startCharging(qint64 userId, qint64 reservationId) const
{
    return fromRepository(
        chargingRepository_->startCharging(userId, reservationId, utcNow(clock_)));
}

ChargingOperationResult ChargingService::chargingStatus(qint64 userId, qint64 orderId) const
{
    const QDateTime now = utcNow(clock_);
    ChargingOperationResult result =
        fromRepository(chargingRepository_->chargingStatus(userId, orderId, now));
    if (!result.success || result.order.status != charging::model::OrderStatus::Charging) {
        return result;
    }
    if (!result.order.startedAtUtc.isValid() || now < result.order.startedAtUtc) {
        result.success = false;
        result.error = makeError(charging::protocol::error_code::kInternalError,
                                 QStringLiteral("订单的充电开始时间无效"));
        return result;
    }

    const BillingResult billing =
        billingService_->calculate(result.charger.powerWatts, result.order.startedAtUtc.secsTo(now),
                                   result.order.unitPriceCentsPerKwh);
    if (!billing.success) {
        result.success = false;
        result.error = billing.error;
        return result;
    }
    result.order.durationSeconds = billing.durationSeconds;
    result.order.energyWh = billing.energyWh;
    result.order.amountCents = billing.amountCents;
    result.currentPowerWatts = result.charger.powerWatts;
    return result;
}

ChargingOperationResult ChargingService::stopCharging(qint64 userId, qint64 orderId) const
{
    const QDateTime now = utcNow(clock_);
    const ChargingRepositoryResult snapshot =
        chargingRepository_->chargingStatus(userId, orderId, now);
    ChargingOperationResult current = fromRepository(snapshot);
    if (!current.success) {
        return current;
    }

    if (current.order.status == charging::model::OrderStatus::WaitingPayment ||
        current.order.status == charging::model::OrderStatus::Completed) {
        current.idempotent = true;
        current.currentPowerWatts = 0;
        return current;
    }
    if (current.order.status != charging::model::OrderStatus::Charging ||
        !ChargingStateMachine::canTransition(current.order.status,
                                             charging::model::OrderStatus::WaitingPayment) ||
        !current.order.startedAtUtc.isValid() || now < current.order.startedAtUtc) {
        current.success = false;
        current.error = makeError(charging::protocol::error_code::kInvalidStateTransition,
                                  QStringLiteral("当前订单不允许停止充电"));
        return current;
    }

    const BillingResult billing = billingService_->calculate(current.charger.powerWatts,
                                                             current.order.startedAtUtc.secsTo(now),
                                                             current.order.unitPriceCentsPerKwh);
    if (!billing.success) {
        current.success = false;
        current.error = billing.error;
        return current;
    }

    return fromRepository(chargingRepository_->stopCharging(
        userId, orderId, current.order.startedAtUtc, now, billing.durationSeconds, billing.energyWh,
        billing.amountCents));
}

ChargingOperationResult ChargingService::fromRepository(const ChargingRepositoryResult& value) const
{
    ChargingOperationResult result;
    result.success = value.ok;
    result.idempotent = value.idempotent;
    result.reservation = value.reservation;
    result.order = value.order;
    result.charger = value.charger;
    if (value.ok) {
        result.currentPowerWatts = value.order.status == charging::model::OrderStatus::Charging
                                       ? value.charger.powerWatts
                                       : 0;
    } else {
        result.error = mapRepositoryError(value.error, value.diagnostic);
    }
    return result;
}

} // namespace charging::server
