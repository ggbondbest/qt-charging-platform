#include "charging/client/profile_charging/wallet_service.h"

#include "charging/common/model/model_json.h"
#include "charging/common/protocol/protocol.h"

#include <QJsonArray>
#include <QJsonObject>

namespace charging::client {

namespace {

charging::protocol::ProtocolError makeLocalError(const char* code, const QString& message)
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(code);
    error.message = message;
    return error;
}

} // namespace

WalletService::WalletService(IRequestTransport* transport, QObject* parent)
    : QObject(parent), transport_(transport)
{
}

bool WalletService::isFetchingProfile() const
{
    return fetchingProfile_;
}

bool WalletService::isRecharging() const
{
    return recharging_;
}

bool WalletService::isFetchingRecords() const
{
    return fetchingRecords_;
}

void WalletService::fetchProfile()
{
    if (fetchingProfile_) {
        return; // Duplicate submissions are ignored at the service boundary.
    }
    if (transport_ == nullptr) {
        emit operationFailed(QString::fromLatin1(charging::protocol::request_type::kGetUserInfo),
                             makeLocalError(charging::protocol::error_code::kInternalError,
                                            QStringLiteral("transport is not installed")));
        return;
    }

    fetchingProfile_ = true;
    transport_->send(QString::fromLatin1(charging::protocol::request_type::kGetUserInfo),
                     QJsonObject{},
                     [this](bool success, const QJsonObject& data,
                            const charging::protocol::ProtocolError& error) {
                         fetchingProfile_ = false;
                         const QString type = QString::fromLatin1(
                             charging::protocol::request_type::kGetUserInfo);
                         if (!success) {
                             emit operationFailed(type, error);
                             return;
                         }
                         charging::model::User user;
                         QString parseError;
                         if (!charging::model::fromJson(data.value(QStringLiteral("user"))
                                                            .toObject(),
                                                        &user, &parseError)) {
                             emit operationFailed(
                                 type, makeLocalError(
                                            charging::protocol::error_code::kInternalError,
                                            QStringLiteral("invalid user payload: ")
                                                + parseError));
                             return;
                         }
                         emit profileLoaded(user);
                     });
}

void WalletService::recharge(qint64 amountCents)
{
    const QString type =
        QString::fromLatin1(charging::protocol::request_type::kRecharge);
    if (recharging_) {
        return; // A recharge is in flight; never send a second one.
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }
    if (amountCents <= 0 || amountCents > kMaximumRechargeCents) {
        // Local pre-check only mirrors documented bounds; the server stays
        // authoritative once real interfaces are released.
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInvalidEnvelope,
                                                  QStringLiteral("充值金额无效")));
        return;
    }

    recharging_ = true;
    QJsonObject payload;
    payload.insert(QStringLiteral("amountCents"), amountCents);
    transport_->send(
        type, payload,
        [this, amountCents](bool success, const QJsonObject& data,
                            const charging::protocol::ProtocolError& error) {
            recharging_ = false;
            const QString rechargeType =
                QString::fromLatin1(charging::protocol::request_type::kRecharge);
            if (!success) {
                emit operationFailed(rechargeType, error);
                return;
            }
            const qint64 balanceAfterCents =
                data.value(QStringLiteral("balanceAfterCents")).toDouble();
            emit rechargeCompleted(amountCents, balanceAfterCents);
        });
}

void WalletService::fetchRechargeRecords(int page)
{
    const QString type =
        QString::fromLatin1(charging::protocol::request_type::kGetRechargeRecords);
    if (fetchingRecords_) {
        return;
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }
    const int safePage = page > 0 ? page : 1;

    fetchingRecords_ = true;
    QJsonObject payload;
    // TODO(contract): page/pageSize/total naming is not frozen in
    // docs/api/socket_protocol.md yet; align with the leader before wiring
    // the real transport.
    payload.insert(QStringLiteral("page"), safePage);
    payload.insert(QStringLiteral("pageSize"), kRechargePageSize);
    transport_->send(
        type, payload,
        [this, safePage](bool success, const QJsonObject& data,
                         const charging::protocol::ProtocolError& error) {
            fetchingRecords_ = false;
            const QString recordsType =
                QString::fromLatin1(charging::protocol::request_type::kGetRechargeRecords);
            if (!success) {
                emit operationFailed(recordsType, error);
                return;
            }

            QVector<charging::model::RechargeRecord> records;
            const QJsonArray array = data.value(QStringLiteral("records")).toArray();
            for (const QJsonValue& value : array) {
                charging::model::RechargeRecord record;
                QString parseError;
                if (!charging::model::fromJson(value.toObject(), &record, &parseError)) {
                    emit operationFailed(
                        recordsType, makeLocalError(charging::protocol::error_code::kInternalError,
                                                    QStringLiteral("invalid record payload: ")
                                                        + parseError));
                    return;
                }
                records.append(record);
            }

            const int total = data.value(QStringLiteral("total")).toInt();
            const bool hasMore = safePage * kRechargePageSize < total;
            emit rechargeRecordsLoaded(records, total, hasMore);
        });
}

} // namespace charging::client
