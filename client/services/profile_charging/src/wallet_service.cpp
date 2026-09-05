#include "charging/client/profile_charging/wallet_service.h"

#include "charging/common/model/model_json.h"
#include "charging/common/protocol/protocol.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>
#include <cmath>
#include "network/page_validation.h"

namespace charging::client {

namespace {

charging::protocol::ProtocolError makeLocalError(const char* code, const QString& message)
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(code);
    error.message = message;
    return error;
}

// 本地预检与契约 v1 服务端校验同码同形：INVALID_ARGUMENT + details.field，
// 页面经 displayMessageForError 按字段映射文案，本地/远端拒绝表现一致。
charging::protocol::ProtocolError makeLocalFieldError(const QString& field)
{
    charging::protocol::ProtocolError error = makeLocalError(
        charging::protocol::error_code::kInvalidArgument,
        QStringLiteral("Invalid user API request: ") + field);
    error.details.insert(QStringLiteral("field"), field);
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

bool WalletService::isUpdatingNickname() const
{
    return updatingNickname_;
}

bool WalletService::isUpdatingAvatar() const
{
    return updatingAvatar_;
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
    transport_->sendFor(this, QString::fromLatin1(charging::protocol::request_type::kGetUserInfo),
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

void WalletService::updateNickname(const QString& nickname)
{
    const QString type =
        QString::fromLatin1(charging::protocol::request_type::kUpdateUserInfo);
    if (updatingNickname_) {
        return; // An update is in flight; never send a second one.
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }
    const QString trimmed = nickname.trimmed();
    if (trimmed.isEmpty() || trimmed.length() > 32) {
        // Local pre-check mirrors the frozen contract bound (trim(nickname)
        // is 1..32, §3 UPDATE_USER_INFO); the server stays authoritative.
        emit operationFailed(type, makeLocalFieldError(QStringLiteral("nickname")));
        return;
    }

    updatingNickname_ = true;
    QJsonObject payload;
    // Frozen in docs/api/user_api_contract.md; production uses the live adapter.
    payload.insert(QStringLiteral("nickname"), trimmed);
    transport_->sendFor(this,
        type, payload,
        [this](bool success, const QJsonObject& data,
               const charging::protocol::ProtocolError& error) {
            updatingNickname_ = false;
            const QString updateType = QString::fromLatin1(
                charging::protocol::request_type::kUpdateUserInfo);
            if (!success) {
                emit operationFailed(updateType, error);
                return;
            }
            charging::model::User user;
            QString parseError;
            if (!charging::model::fromJson(data.value(QStringLiteral("user")).toObject(),
                                           &user, &parseError)) {
                emit operationFailed(
                    updateType, makeLocalError(
                                   charging::protocol::error_code::kInternalError,
                                   QStringLiteral("invalid user payload: ") + parseError));
                return;
            }
            emit profileLoaded(user);
        });
}

void WalletService::updateAvatar(const QString& avatarKey)
{
    const QString type =
        QString::fromLatin1(charging::protocol::request_type::kUpdateUserInfo);
    if (updatingAvatar_ || updatingNickname_) {
        return; // One profile write in flight at a time.
    }
    if (transport_ == nullptr) {
        emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                  QStringLiteral("transport is not installed")));
        return;
    }

    updatingAvatar_ = true;
    QJsonObject payload;
    // Frozen avatarKey patch: built-in keys only; empty restores the default.
    // Image upload is not part of docs/api/user_api_contract.md.
    payload.insert(QStringLiteral("avatarKey"), avatarKey);
    transport_->sendFor(this,
        type, payload,
        [this](bool success, const QJsonObject& data,
               const charging::protocol::ProtocolError& error) {
            updatingAvatar_ = false;
            const QString updateType = QString::fromLatin1(
                charging::protocol::request_type::kUpdateUserInfo);
            if (!success) {
                emit operationFailed(updateType, error);
                return;
            }
            charging::model::User user;
            QString parseError;
            if (!charging::model::fromJson(data.value(QStringLiteral("user")).toObject(),
                                           &user, &parseError)) {
                emit operationFailed(
                    updateType, makeLocalError(
                                   charging::protocol::error_code::kInternalError,
                                   QStringLiteral("invalid user payload: ") + parseError));
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
    if (amountCents < 1 || amountCents > kMaximumRechargeCents) {
        // Local pre-check mirrors the frozen contract bound
        // (1..10000000 cents, §3 RECHARGE); the server stays authoritative.
        emit operationFailed(type, makeLocalFieldError(QStringLiteral("amountCents")));
        return;
    }

    const QString scope = transport_->persistenceScope();
    const QString settingsKey = QStringLiteral("pendingRecharge/") + scope;
    QSettings settings;
    QJsonObject payload;
    if (!scope.isEmpty() && settings.contains(settingsKey)) {
        payload = QJsonDocument::fromJson(settings.value(settingsKey).toByteArray()).object();
        if (payload.value("transactionNo").toString().isEmpty()
            || payload.value("amountCents").toDouble() != double(amountCents)) {
            charging::protocol::ProtocolError error = makeLocalError(
                charging::protocol::error_code::kInvalidArgument,
                tr("存在结果未确认的充值，请使用原金额重试，勿发起新交易"));
            error.details.insert(QStringLiteral("field"), QStringLiteral("pendingRecharge"));
            emit operationFailed(type, error);
            return;
        }
    } else {
        payload.insert("amountCents", static_cast<double>(amountCents));
        payload.insert("transactionNo", QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!scope.isEmpty()) {
            settings.setValue(settingsKey, QJsonDocument(payload).toJson(QJsonDocument::Compact));
            settings.sync();
            if (settings.status() != QSettings::NoError) {
                emit operationFailed(type, makeLocalError(charging::protocol::error_code::kInternalError,
                                                          tr("无法保存充值流水，未发送充值请求")));
                return;
            }
        }
    }
    recharging_ = true;
    transport_->sendFor(this,
        type, payload,
        [this, amountCents, scope, settingsKey, payload](bool success, const QJsonObject& data,
                            const charging::protocol::ProtocolError& error) {
            recharging_ = false;
            const QString rechargeType =
                QString::fromLatin1(charging::protocol::request_type::kRecharge);
            if (!success) {
                // A transport failure is an unknown outcome, not a failed recharge.
                if (!scope.isEmpty() && (error.code == QLatin1String(charging::protocol::error_code::kRechargeFailed)
                    || error.code == QLatin1String(charging::protocol::error_code::kIdempotencyConflict)
                    || error.code == QLatin1String(charging::protocol::error_code::kInvalidArgument))) {
                    QSettings saved; saved.remove(settingsKey); saved.sync();
                }
                emit operationFailed(rechargeType, error);
                return;
            }
            const QJsonValue balance = data.value("balanceCents");
            const double number = balance.toDouble(-1);
            charging::model::RechargeRecord record;
            const bool validRecord = charging::model::fromJson(data.value("record").toObject(), &record);
            if (!balance.isDouble() || number < 0 || !std::isfinite(number)
                || std::floor(number) != number || number > double(charging::model::kMaximumJsonSafeInteger)
                || !validRecord || record.status != charging::model::RechargeStatus::Success
                || record.amountCents != amountCents || record.transactionNo != payload.value("transactionNo").toString()
                || !data.value("idempotent").isBool()) {
                emit operationFailed(rechargeType, makeLocalError(charging::protocol::error_code::kInvalidEnvelope,
                    tr("充值结果格式错误，请使用原金额重试确认")));
                return;
            }
            if (!scope.isEmpty()) { QSettings saved; saved.remove(settingsKey); saved.sync(); }
            const qint64 balanceAfterCents = static_cast<qint64>(number);
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
    // Pagination names are frozen in docs/api/user_api_contract.md.
    payload.insert(QStringLiteral("page"), safePage);
    payload.insert(QStringLiteral("pageSize"), kRechargePageSize);
    transport_->sendFor(this,
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

            bool hasMore = false;
            if (!network::readPage(data, "records", safePage, kRechargePageSize, &hasMore)) {
                emit operationFailed(recordsType, makeLocalError(charging::protocol::error_code::kInvalidEnvelope,
                                                                 tr("充值记录分页响应无效")));
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
            emit rechargeRecordsLoaded(records, total, hasMore);
        });
}

qint64 WalletService::pendingRechargeAmount() const
{
    if (transport_ == nullptr) {
        return 0;
    }
    const QString scope = transport_->persistenceScope();
    if (scope.isEmpty()) {
        return 0; // 预览/无连接通道不落盘意图，也就没有未确认状态。
    }
    QSettings settings;
    const QByteArray raw = settings.value(QStringLiteral("pendingRecharge/") + scope)
                               .toByteArray();
    const QJsonObject intent = QJsonDocument::fromJson(raw).object();
    if (intent.value(QStringLiteral("transactionNo")).toString().isEmpty()) {
        return 0;
    }
    return static_cast<qint64>(intent.value(QStringLiteral("amountCents")).toDouble());
}

} // namespace charging::client
