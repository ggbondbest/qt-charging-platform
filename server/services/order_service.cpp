#include "order_service.h"

#include "order_repository.h"
#include "workflow_repository_types.h"

#include <QDebug>

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
        return makeError(error_code::kNotFound, QStringLiteral("订单不存在"));
    case RepositoryError::Unauthorized:
        return makeError(error_code::kUnauthorized, QStringLiteral("无权操作该订单"));
    case RepositoryError::InsufficientBalance:
        return makeError(error_code::kInsufficientBalance, QStringLiteral("账户余额不足"));
    case RepositoryError::InvalidStateTransition:
        return makeError(error_code::kInvalidStateTransition,
                         QStringLiteral("当前订单状态不允许支付"));
    case RepositoryError::Database:
        qWarning().noquote() << "Order payment database operation failed";
        return makeError(error_code::kDatabaseError, QStringLiteral("数据库操作失败，请稍后重试"));
    case RepositoryError::ArithmeticOverflow:
        return makeError(error_code::kInternalError, QStringLiteral("订单金额超出系统范围"));
    case RepositoryError::UserFrozen:
        return makeError(error_code::kUserFrozen, QStringLiteral("该用户已被冻结"));
    case RepositoryError::ChargerNotAvailable:
    case RepositoryError::ExistingUnfinishedOrder:
    case RepositoryError::None:
        break;
    }
    return makeError(error_code::kInternalError, QStringLiteral("服务器内部状态异常"));
}

} // namespace

OrderService::OrderService(OrderRepository* orderRepository, UtcClock clock)
    : orderRepository_(orderRepository), clock_(std::move(clock))
{
    Q_ASSERT(orderRepository_ != nullptr);
}

PaymentResult OrderService::pay(qint64 userId, qint64 orderId) const
{
    const OrderRepositoryResult stored = orderRepository_->pay(userId, orderId, utcNow(clock_));
    PaymentResult result;
    result.success = stored.ok;
    result.idempotent = stored.idempotent;
    result.order = stored.order;
    result.balanceCents = stored.balanceCents;
    if (!stored.ok) {
        result.error = mapRepositoryError(stored.error, stored.diagnostic);
    }
    return result;
}

} // namespace charging::server
