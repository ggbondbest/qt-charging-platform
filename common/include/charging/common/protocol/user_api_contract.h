#pragma once

#include "charging/common/protocol/protocol.h"

namespace charging::protocol::user_api {

inline constexpr int kDefaultPage = 1;
inline constexpr int kDefaultPageSize = 20;
inline constexpr int kMaximumPageSize = 100;
inline constexpr int kMaximumPage = 2147483647;
inline constexpr qint64 kMaximumRechargeCents = 10000000;
inline constexpr int kMaximumStatsMonths = 12;
inline constexpr int kMaximumAvatarBytes = 128 * 1024;
inline constexpr int kMaximumAvatarDimension = 512;
// Recharge-reward coupon rule (2026-09-08 proposal; TODO(contract): business
// sign-off pending — thresholds live here so the team can change them in one place):
// one ¥5.00 charging coupon per SUCCESS recharge of at least ¥50.00, 30-day validity.
inline constexpr qint64 kCouponRechargeThresholdCents = 5000;
inline constexpr qint64 kCouponValueCents = 500;
inline constexpr int kCouponValidityDays = 30;
// Daily check-in reward (2026-09-08; TODO(contract): business sign-off pending —
// amount lives here so the team can change it in one place).
// One check-in per user per UTC day, idempotent; grants kCheckInRewardPoints.
inline constexpr qint64 kCheckInRewardPoints = 10;
// Settlement reward (2026-09-09 proposal; TODO(contract): business sign-off
// pending — rate lives here so the team can change it in one place): every
// full yuan paid on an order settlement grants kSettlementPointsPerYuan points
// (floor division: ¥48.64 pays 48 points). Ledger reason 'SETTLEMENT',
// display-mapped to "消费返积分" on the GET_POINTS output side. Computed once
// in settlementRewardPoints() so server and mock share the exact rule.
inline constexpr qint64 kSettlementPointsPerYuan = 1;
inline constexpr qint64 settlementRewardPoints(qint64 amountCents)
{
    return amountCents <= 0 ? 0 : (amountCents / 100) * kSettlementPointsPerYuan;
}
// Level-up gift reward (2026-09-09 需求批, 2026-09-09 用户拍板改到账: 原方案礼包
// 只记客户端等级账目；现礼包积分真入账 points_ledger——CREDIT_LEVEL_REWARD 只带
// level，金额由本函数单点推导，客户端传额不被信任)。TODO(contract): 业务定稿待评审；
// 金额与客户端 ProgressService kTiers 的 giftPoints 互指镜像（tst_qml_client_pages
// 钉一致性，改动必然撞测试）。Ledger reason 'LEVEL_GIFT'，GET_POINTS 输出侧映射
// "等级礼包"；幂等键 (user_id, 'LEVEL_GIFT', amount)——各档金额互不相同天然可辨。
inline constexpr int kMinimumLevel = 2;   // 青铜(Lv.1)为初始档，无礼包可发
inline constexpr int kMaximumLevel = 5;
inline constexpr qint64 levelRewardPoints(int level)
{
    switch (level) {
    case 2: return 100;    // 白银
    case 3: return 150;    // 黄金
    case 4: return 200;    // 铂金
    case 5: return 300;    // 黑金
    default: return 0;
    }
}
// Charger rating (2026-09-08): 1..5 stars, optional comment capped at 140
// code points. TODO(contract): whether to allow editing a submitted rating
// (one rating per order, immutable once submitted).
inline constexpr int kMinimumRating = 1;
inline constexpr int kMaximumRating = 5;
inline constexpr int kMaximumRatingCommentChars = 140;

// Validates only the sixteen user API request-data contracts documented in
// docs/api/user_api_contract.md. This does NOT authenticate, query SQL, or
// register a Dispatcher handler. The caller must obtain identity from Session.
// On success, defaults are inserted, strings are normalized, and unknown keys
// (including userId) are discarded. On failure, normalized is unchanged.
// Both output pointers may be null. Unknown actions return UNKNOWN_REQUEST_TYPE.
bool normalizeRequestData(const QString& type, const QJsonObject& data,
                          QJsonObject* normalized, ProtocolError* error = nullptr);

} // namespace charging::protocol::user_api
