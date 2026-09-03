#pragma once

#include "charging/common/model/models.h"
#include "charging/common/protocol/protocol.h"

#include <QString>

namespace charging::server {

class UserRepository;

struct LoginResult
{
    bool success = false;
    bool created = false;
    charging::model::User user;
    charging::protocol::ProtocolError error;
};

class UserService final
{
public:
    explicit UserService(UserRepository* userRepository);

    LoginResult loginOrRegister(const QString& phone) const;

private:
    UserRepository* userRepository_ = nullptr;
};

} // namespace charging::server
