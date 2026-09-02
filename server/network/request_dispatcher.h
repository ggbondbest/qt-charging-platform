#pragma once

#include "charging/common/protocol/protocol.h"

#include <QtGlobal>

namespace charging::server {

class UserService;

class RequestDispatcher final
{
public:
    explicit RequestDispatcher(UserService* userService);

    charging::protocol::ResponseEnvelope dispatch(
        const charging::protocol::RequestEnvelope& request,
        qint64* authenticatedUserId = nullptr) const;

private:
    UserService* userService_ = nullptr;
};

} // namespace charging::server
