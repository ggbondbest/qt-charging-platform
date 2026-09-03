#include "charging/client/profile_charging/order_status_display.h"

namespace charging::client {

OrderStatusDisplay orderStatusDisplay(charging::model::OrderStatus status)
{
    using Status = charging::model::OrderStatus;
    switch (status) {
    case Status::Reserved:
        return {QStringLiteral("已预约"), StatusTag::Tone::Info};
    case Status::Charging:
        return {QStringLiteral("充电中"), StatusTag::Tone::Success};
    case Status::WaitingPayment:
        return {QStringLiteral("待支付"), StatusTag::Tone::Warning};
    case Status::Completed:
        return {QStringLiteral("已完成"), StatusTag::Tone::Neutral};
    case Status::Cancelled:
        return {QStringLiteral("已取消"), StatusTag::Tone::Neutral};
    }
    // Unknown future enum value: show something neutral, never crash.
    return {QStringLiteral("未知状态"), StatusTag::Tone::Neutral};
}

} // namespace charging::client
