#include "charging_service.h"

#include "billing_service.h"
#include "charging_repository.h"
#include "charging_target_repository.h"
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

ChargingOperationResult ChargingService::startCharging(qint64 userId, qint64 reservationId,
                                                       const QJsonObject& target) const
{
    if (!validateChargingTarget(target)) {
        ChargingOperationResult result;
        result.error = makeError(charging::protocol::error_code::kInvalidEnvelope,
                                 QStringLiteral("请选择有效的充电目标和正整数目标值"));
        return result;
    }
    return fromRepository(
        chargingRepository_->startCharging(userId, reservationId, utcNow(clock_), target));
}

ChargingOperationResult ChargingService::chargingStatus(qint64 userId, qint64 orderId) const
{
    return sampleCharging(userId, orderId, false);
}

ChargingOperationResult ChargingService::stopCharging(qint64 userId, qint64 orderId) const
{
    return sampleCharging(userId, orderId, true);
}

ChargingOperationResult ChargingService::sampleCharging(qint64 userId, qint64 orderId,
                                                        bool manualStop) const
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
        current.idempotent = manualStop;
        current.currentPowerWatts = 0;
        return current;
    }
    if (current.order.status != charging::model::OrderStatus::Charging ||
        !ChargingStateMachine::canTransition(current.order.status,
                                             charging::model::OrderStatus::WaitingPayment) ||
        !current.order.startedAtUtc.isValid() || now < current.order.startedAtUtc) {
        if (!manualStop && current.order.status != charging::model::OrderStatus::Charging)
            return current;
        current.success = false;
        current.error = makeError(charging::protocol::error_code::kInvalidStateTransition,
                                  QStringLiteral("当前订单不允许停止充电"));
        return current;
    }

    BillingResult billing;
    QString stopReason = QStringLiteral("MANUAL");
    bool reached = false;
    if (!current.order.target.isEmpty()) {
        const TargetMeterSample sample = calculateTargetSample(
            current.charger.powerWatts, current.order.startedAtUtc.secsTo(now),
            current.order.unitPriceCentsPerKwh, current.order.target);
        billing.success = sample.success;
        billing.durationSeconds = sample.durationSeconds;
        billing.energyWh = sample.energyWh;
        billing.amountCents = sample.amountCents;
        reached = sample.reached;
        if (reached) stopReason = sample.stopReason;
        if (!sample.success)
            billing.error = makeError(charging::protocol::error_code::kInternalError,
                                      QStringLiteral("无法计算当前充电目标的安全计量值"));
    } else {
        billing = billingService_->calculate(current.charger.powerWatts,
                                              current.order.startedAtUtc.secsTo(now),
                                              current.order.unitPriceCentsPerKwh);
    }
    if (!billing.success) {
        current.success = false;
        current.error = billing.error;
        return current;
    }

    if (manualStop || reached) {
        return fromRepository(chargingRepository_->stopCharging(
            userId, orderId, current.order.startedAtUtc, now, billing.durationSeconds,
            billing.energyWh, billing.amountCents, stopReason));
    }
    QString diagnostic;
    if (!chargingRepository_->recordTelemetry(userId, orderId, now, current.charger.powerWatts,
                                              billing.durationSeconds, billing.energyWh,
                                              billing.amountCents, &diagnostic)) {
        current.success = false;
        current.error = mapRepositoryError(RepositoryError::Database, diagnostic);
        return current;
    }
    current.order.durationSeconds = billing.durationSeconds;
    current.order.energyWh = billing.energyWh;
    current.order.amountCents = billing.amountCents;
    if (!chargingTargetDto(chargingRepository_->database(), orderId, &current.order.target,
                           &current.order.stopReason, &diagnostic)) {
        current.success = false;
        current.error = mapRepositoryError(RepositoryError::Database, diagnostic);
    }
    return current;
}

bool ChargingService::advanceTargets(QString* diagnostic) const
{
    if (diagnostic) diagnostic->clear();
    QVector<QPair<qint64, qint64>> orders;
    if (!activeChargingTargetOrders(chargingRepository_->database(), &orders, diagnostic))
        return false;
    bool success = true;
    for (const auto& entry : orders) {
        const ChargingOperationResult result = sampleCharging(entry.first, entry.second, false);
        if (!result.success) {
            success = false;
            if (diagnostic) *diagnostic = QStringLiteral("A target charging sample could not be applied");
        }
    }
    return success;
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
        QString diagnostic;
        if (value.order.id > 0 && !chargingTargetDto(chargingRepository_->database(), value.order.id,
                                                    &result.order.target, &result.order.stopReason,
                                                    &diagnostic)) {
            result.success = false;
            result.error = mapRepositoryError(RepositoryError::Database, diagnostic);
            return result;
        }
        result.currentPowerWatts = value.order.status == charging::model::OrderStatus::Charging
                                       ? value.charger.powerWatts
                                       : 0;
    } else {
        result.error = mapRepositoryError(value.error, value.diagnostic);
        if (value.error == RepositoryError::ExistingUnfinishedOrder && value.order.id > 0) {
            // Retain the existing error code for older clients. These safe,
            // session-owned identifiers let QML recover the blocking workflow.
            result.error.details.insert(QStringLiteral("reason"), QStringLiteral("UNFINISHED_ORDER"));
            result.error.details.insert(QStringLiteral("orderId"), QString::number(value.order.id));
            result.error.details.insert(QStringLiteral("reservationId"), QString::number(value.order.reservationId));
            result.error.details.insert(QStringLiteral("status"), charging::model::toString(value.order.status));
        }
    }
    return result;
}

} // namespace charging::server
