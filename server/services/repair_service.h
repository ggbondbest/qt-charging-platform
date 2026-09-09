#pragma once

#include "service_clock.h"
#include <QJsonObject>

namespace charging::server {
class RepairRepository;

class RepairService final
{
public:
    explicit RepairService(RepairRepository* repository, UtcClock clock = {});
    static bool handles(const QString& type);
    static bool handlesAdmin(const QString& action);
    QJsonObject handle(const QString& type, const QJsonObject& parameters, qint64 userId) const;
    // AdminService must validate the administrator session before this call.
    // The repository rechecks ACTIVE status inside the write transaction.
    QJsonObject adminHandle(const QString& action, const QJsonObject& parameters, qint64 adminId) const;

private:
    RepairRepository* repository_ = nullptr;
    UtcClock clock_;
};
} // namespace charging::server
