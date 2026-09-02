#include "services/station/auth_service.h"

#include "charging/common/model/model_json.h"
#include "network/client_connection.h"

#include <QJsonObject>
#include <QRegularExpression>

namespace charging::client::services::station {

AuthService::AuthService(network::ClientConnection* connection, QObject* parent)
    : QObject(parent), connection_(connection)
{
    Q_ASSERT(connection_ != nullptr);
    connect(connection_, &network::ClientConnection::responseReceived, this,
            &AuthService::handleResponse);
    connect(connection_, &network::ClientConnection::requestFailed, this,
            &AuthService::handleRequestFailure);
}

void AuthService::login(const QString& phone)
{
    if (!pendingLoginRequestId_.isEmpty()) {
        emit loginFailed(tr("登录请求正在处理中，请稍候"));
        return;
    }

    static const QRegularExpression phonePattern(QStringLiteral("^1[0-9]{10}$"));
    const QString normalizedPhone = phone.trimmed();
    if (!phonePattern.match(normalizedPhone).hasMatch()) {
        emit loginFailed(tr("手机号必须为11位数字且以1开头"));
        return;
    }

    QJsonObject data;
    data.insert(QStringLiteral("phone"), normalizedPhone);
    pendingLoginRequestId_ = connection_->sendRequest(
        QString::fromLatin1(charging::protocol::request_type::kUserLogin), data);
    emit loginStarted();
}

bool AuthService::isLoginPending() const
{
    return !pendingLoginRequestId_.isEmpty();
}

void AuthService::handleResponse(const charging::protocol::ResponseEnvelope& response)
{
    if (response.requestId != pendingLoginRequestId_ ||
        response.type != QString::fromLatin1(charging::protocol::request_type::kUserLogin)) {
        return;
    }
    pendingLoginRequestId_.clear();

    if (!response.success) {
        const QString message = response.error.message.isEmpty() ? tr("登录失败")
                                                                  : response.error.message;
        emit loginFailed(message);
        return;
    }

    const QJsonValue createdValue = response.data.value(QStringLiteral("created"));
    const QJsonValue userValue = response.data.value(QStringLiteral("user"));
    if (!createdValue.isBool() || !userValue.isObject()) {
        emit loginFailed(tr("服务端返回的登录数据不完整"));
        return;
    }

    charging::model::User user;
    QString parseError;
    if (!charging::model::fromJson(userValue.toObject(), &user, &parseError)) {
        emit loginFailed(tr("无法读取用户信息：%1").arg(parseError));
        return;
    }
    emit loginSucceeded(user, createdValue.toBool());
}

void AuthService::handleRequestFailure(const QString& requestId, const QString& errorCode,
                                       const QString& message)
{
    Q_UNUSED(errorCode)
    if (requestId != pendingLoginRequestId_) {
        return;
    }
    pendingLoginRequestId_.clear();
    emit loginFailed(message.isEmpty() ? tr("登录请求失败") : message);
}

} // namespace charging::client::services::station
