#pragma once

#include "charging/common/protocol/protocol.h"

namespace charging::protocol::user_api {

inline constexpr int kDefaultPage = 1;
inline constexpr int kDefaultPageSize = 20;
inline constexpr int kMaximumPageSize = 100;
inline constexpr int kMaximumPage = 2147483647;
inline constexpr qint64 kMaximumRechargeCents = 10000000;
inline constexpr int kMaximumStatsMonths = 12;
// Recharge-reward coupon rule (2026-09-08 proposal; TODO(contract): business
// sign-off pending — thresholds live here so the team can change them in one place):
// one ¥5.00 charging coupon per SUCCESS recharge of at least ¥50.00, 30-day validity.
inline constexpr qint64 kCouponRechargeThresholdCents = 5000;
inline constexpr qint64 kCouponValueCents = 500;
inline constexpr int kCouponValidityDays = 30;

// Validates only the eleven user API request-data contracts documented in
// docs/api/user_api_contract.md. This does NOT authenticate, query SQL, or
// register a Dispatcher handler. The caller must obtain identity from Session.
// On success, defaults are inserted, strings are normalized, and unknown keys
// (including userId) are discarded. On failure, normalized is unchanged.
// Both output pointers may be null. Unknown actions return UNKNOWN_REQUEST_TYPE.
bool normalizeRequestData(const QString& type, const QJsonObject& data,
                          QJsonObject* normalized, ProtocolError* error = nullptr);

} // namespace charging::protocol::user_api
