#include "request_dispatcher.h"

#include "charging/common/model/model_json.h"
#include "charging/common/protocol/protocol.h"
#include "user_service.h"

#include <QJsonObject>
#include <QJsonValue>

namespace charging::server {

RequestDispatcher::RequestDispatcher(UserService* userService) : userService_(userService)
{
    Q_ASSERT(userService_ != nullptr);
}

charging::protocol::ResponseEnvelope RequestDispatcher::dispatch(
    const charging::protocol::RequestEnvelope& request, qint64* authenticatedUserId) const
{
    if (request.type ==
        QString::fromLatin1(charging::protocol::request_type::kUserLogin)) {
        const QJsonValue phoneValue = request.data.value(QStringLiteral("phone"));
        const QString phone = phoneValue.isString() ? phoneValue.toString() : QString();
        const LoginResult login = userService_->loginOrRegister(phone);
        if (!login.success) {
            return charging::protocol::makeErrorResponse(request, login.error);
        }

        if (authenticatedUserId != nullptr) {
            *authenticatedUserId = login.user.id;
        }
        QJsonObject responseData;
        responseData.insert(QStringLiteral("created"), login.created);
        responseData.insert(QStringLiteral("user"), charging::model::toJson(login.user));
        return charging::protocol::makeSuccessResponse(request, responseData);
    }

    charging::protocol::ProtocolError error;
    error.code =
        QString::fromLatin1(charging::protocol::error_code::kUnknownRequestType);
    error.message = QStringLiteral("服务端尚未实现请求类型：%1").arg(request.type);
    error.details.insert(QStringLiteral("type"), request.type);
    return charging::protocol::makeErrorResponse(request, error);
}

} // namespace charging::server
