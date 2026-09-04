#pragma once

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/model/models.h"

#include <QObject>
#include <QVector>

namespace charging::client {

// Wallet use cases exposed to pages: profile/balance, recharge and recharge
// records. All results are asynchronous; the page renders whatever the
// transport reports and never decides business outcomes itself.
//
// Wire types used here: GET_USER_INFO, RECHARGE, GET_RECHARGE_RECORDS from
// the candidate-v1 action registry.
class WalletService final : public QObject
{
    Q_OBJECT

public:
    static constexpr int kRechargePageSize = 10;
    static constexpr qint64 kMaximumRechargeCents = 10000000LL; // 99999.99 元

    explicit WalletService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isFetchingProfile() const;
    bool isUpdatingNickname() const;
    bool isRecharging() const;
    bool isFetchingRecords() const;

    void fetchProfile();                    // GET_USER_INFO
    void updateNickname(const QString& nickname); // UPDATE_USER_INFO
    void recharge(qint64 amountCents);      // RECHARGE
    void fetchRechargeRecords(int page);    // GET_RECHARGE_RECORDS, page from 1

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
    bool recharging_ = false;
    bool fetchingRecords_ = false;
};

} // namespace charging::client
