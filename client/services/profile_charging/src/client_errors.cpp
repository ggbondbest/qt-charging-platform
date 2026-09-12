#include "charging/client/profile_charging/client_errors.h"

namespace charging::client {

using charging::protocol::error_code::kChargerNotAvailable;
using charging::protocol::error_code::kConnectionError;
using charging::protocol::error_code::kDatabaseError;
using charging::protocol::error_code::kIdempotencyConflict;
using charging::protocol::error_code::kInsufficientBalance;
using charging::protocol::error_code::kInternalError;
using charging::protocol::error_code::kInvalidArgument;
using charging::protocol::error_code::kInvalidEnvelope;
using charging::protocol::error_code::kInvalidFrame;
using charging::protocol::error_code::kInvalidJson;
using charging::protocol::error_code::kInvalidPhone;
using charging::protocol::error_code::kInvalidStateTransition;
using charging::protocol::error_code::kNotFound;
using charging::protocol::error_code::kPayloadTooLarge;
using charging::protocol::error_code::kRechargeFailed;
using charging::protocol::error_code::kRequestTimeout;
using charging::protocol::error_code::kUnauthorized;
using charging::protocol::error_code::kUnknownRequestType;
using charging::protocol::error_code::kUnsupportedProtocolVersion;
using charging::protocol::error_code::kUserFrozen;

QString displayMessageForError(const charging::protocol::ProtocolError& error)
{
    if (error.code == QLatin1String(kConnectionError)) {
        return QStringLiteral("网络连接失败，请检查服务是否已启动后重试");
    }
    if (error.code == QLatin1String(kRequestTimeout)) {
        return QStringLiteral("请求超时，请稍后重试");
    }
    if (error.code == QLatin1String(kInsufficientBalance)) {
        return QStringLiteral("余额不足，请先充值后再支付");
    }
    if (error.code == QLatin1String(kUnauthorized)) {
        return QStringLiteral("登录状态已失效，请重新登录");
    }
    if (error.code == QLatin1String(kUserFrozen)) {
        return QStringLiteral("账号已被冻结，请联系管理员");
    }
    if (error.code == QLatin1String(kNotFound)) {
        return QStringLiteral("记录不存在或已被更新");
    }
    if (error.code == QLatin1String(kInvalidStateTransition)) {
        return QStringLiteral("当前订单状态不允许该操作");
    }
    if (error.code == QLatin1String(kChargerNotAvailable)) {
        return QStringLiteral("电桩当前不可用");
    }
    // 契约 v1 §2/§3：INVALID_ARGUMENT 用 details.field 指出被拒字段，
    // 文案按字段定制（本地预检与真实服务端同码同形）。
    if (error.code == QLatin1String(kInvalidArgument)) {
        const QString field = error.details.value(QStringLiteral("field")).toString();
        if (field == QLatin1String("nickname")) {
            return QStringLiteral("昵称需为 1–32 个字符");
        }
        if (field == QLatin1String("avatarKey")) {
            return QStringLiteral("头像不可用，请从内置头像中选择");
        }
        if (field == QLatin1String("amountCents")) {
            return QStringLiteral("充值金额无效，单笔需为 0.01–100000 元");
        }
        if (field == QLatin1String("transactionNo")) {
            return QStringLiteral("充值请求异常，请稍后重试");
        }
        if (field == QLatin1String("page") || field == QLatin1String("pageSize")) {
            return QStringLiteral("页码无效，请刷新后重试");
        }
        if (field == QLatin1String("pendingRecharge")) {
            return QStringLiteral("存在结果未确认的充值，请使用原金额重试");
        }
        return QStringLiteral("请求参数无效，请检查后重试");
    }
    if (error.code == QLatin1String(kRechargeFailed)) {
        return QStringLiteral("充值未成功，请稍后重试或更换金额");
    }
    if (error.code == QLatin1String(kIdempotencyConflict)) {
        return QStringLiteral("该笔充值与已有交易冲突，请刷新钱包确认余额");
    }
    if (error.code == QLatin1String(kInvalidPhone)) {
        return QStringLiteral("手机号格式不正确");
    }
    if (error.code == QLatin1String(kDatabaseError)
        || error.code == QLatin1String(kInternalError)
        || error.code == QLatin1String(kUnknownRequestType)) {
        return QStringLiteral("服务器繁忙，请稍后重试");
    }
    if (error.code == QLatin1String(kInvalidFrame)
        || error.code == QLatin1String(kPayloadTooLarge)
        || error.code == QLatin1String(kInvalidJson)
        || error.code == QLatin1String(kInvalidEnvelope)
        || error.code == QLatin1String(kUnsupportedProtocolVersion)) {
        return QStringLiteral("请求数据异常，请重试或联系管理员");
    }
    return QStringLiteral("操作失败，请稍后重试");
}

} // namespace charging::client
