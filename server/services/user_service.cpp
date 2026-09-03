#include "user_service.h"

#include "charging/common/model/enums.h"
#include "charging/common/protocol/protocol.h"
#include "user_repository.h"

#include <QDebug>
#include <QJsonObject>
#include <QRegularExpression>

namespace charging::server {

namespace {

charging::protocol::ProtocolError invalidPhoneError()
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kInvalidPhone);
    error.message = QStringLiteral("手机号必须为 11 位数字且以 1 开头");
    error.details.insert(QStringLiteral("field"), QStringLiteral("phone"));
    return error;
}

charging::protocol::ProtocolError databaseError()
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kDatabaseError);
    error.message = QStringLiteral("数据库操作失败，请稍后重试");
    return error;
}

charging::protocol::ProtocolError frozenUserError()
{
    charging::protocol::ProtocolError error;
    error.code = QString::fromLatin1(charging::protocol::error_code::kUserFrozen);
    error.message = QStringLiteral("该用户已被冻结，暂时无法登录");
    return error;
}

} // namespace

UserService::UserService(UserRepository* userRepository) : userRepository_(userRepository)
{
    Q_ASSERT(userRepository_ != nullptr);
}

LoginResult UserService::loginOrRegister(const QString& phone) const
{
    LoginResult result;
    static const QRegularExpression kPhonePattern(QStringLiteral("^1[0-9]{10}$"));
    if (!kPhonePattern.match(phone).hasMatch()) {
        result.error = invalidPhoneError();
        return result;
    }

    const UserLookupResult lookup = userRepository_->findByPhone(phone);
    if (!lookup.ok) {
        qWarning().noquote() << "Unable to look up login user";
        result.error = databaseError();
        return result;
    }

    if (lookup.found) {
        if (lookup.user.status == charging::model::UserStatus::Frozen) {
            result.error = frozenUserError();
            return result;
        }
        result.success = true;
        result.user = lookup.user;
        return result;
    }

    const QString nickname = QStringLiteral("用户") + phone.right(4);
    const UserCreateResult creation = userRepository_->create(phone, nickname);
    if (!creation.ok) {
        qWarning().noquote() << "Unable to auto-register login user";
        result.error = databaseError();
        return result;
    }
    if (creation.user.status == charging::model::UserStatus::Frozen) {
        result.error = frozenUserError();
        return result;
    }

    result.success = true;
    result.created = creation.created;
    result.user = creation.user;
    return result;
}

} // namespace charging::server
