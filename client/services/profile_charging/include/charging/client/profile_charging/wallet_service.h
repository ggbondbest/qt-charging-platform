#pragma once

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/model/models.h"
#include "charging/common/protocol/user_api_contract.h"

#include <QObject>
#include <QVector>

namespace charging::client {

// Wallet use cases exposed to pages: profile/balance, recharge and recharge
// records. All results are asynchronous; the page renders whatever the
// transport reports and never decides business outcomes itself.
//
// Wire types used here: GET_USER_INFO, RECHARGE, GET_RECHARGE_RECORDS from
// contract v1 (docs/api/user_api_contract.md), live via NetworkRequestTransport.
class WalletService final : public QObject
{
    Q_OBJECT

public:
    static constexpr int kRechargePageSize = 10;
    // 契约 v1 §3 冻结的充值上限（1..10000000 分，即 ≤ 100000 元）；直接引用
    // 公共校验器常量保持单一来源。
    static constexpr qint64 kMaximumRechargeCents =
        charging::protocol::user_api::kMaximumRechargeCents;

    explicit WalletService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isFetchingProfile() const;
    bool isUpdatingNickname() const;
    bool isUpdatingAvatar() const;
    bool isRecharging() const;
    bool isFetchingRecords() const;

    void fetchProfile();                    // GET_USER_INFO
    void updateNickname(const QString& nickname); // UPDATE_USER_INFO
    void updateAvatar(const QString& avatarKey);  // UPDATE_USER_INFO（内置头像库选择）
    void recharge(qint64 amountCents);      // RECHARGE
    void fetchRechargeRecords(int page);    // GET_RECHARGE_RECORDS, page from 1
    // 契约 v1 §3 RECHARGE 的"结果未确认"持久化意图（金额分），无未确认充值时
    // 返回 0。超时/断线不是明确失败：恢复必须沿用原金额与流水号重试，服务端
    // 幂等保证不重复入账；服务层不自动重发写操作，提示与重试入口由页面呈现。
    qint64 pendingRechargeAmount() const;

signals:
    void profileLoaded(const charging::model::User& user); // also after UPDATE_USER_INFO
    void rechargeCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void rechargeRecordsLoaded(const QVector<charging::model::RechargeRecord>& records,
                               int total, bool hasMore);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    IRequestTransport* transport_ = nullptr;
    bool fetchingProfile_ = false;
    bool updatingNickname_ = false;
    bool updatingAvatar_ = false;
    bool recharging_ = false;
    bool fetchingRecords_ = false;
};

} // namespace charging::client
