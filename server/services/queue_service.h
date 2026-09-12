#pragma once

#include "service_clock.h"
#include <QJsonObject>

namespace charging::server {
class QueueRepository;

// Identity always comes from the authenticated session, never request data.
class QueueService final
{
public:
    explicit QueueService(QueueRepository* repository, UtcClock clock = {});
    static bool handles(const QString& type);
    QJsonObject handle(const QString& type, const QJsonObject& data, qint64 userId) const;
    // The administrative gateway performs administrator authentication first.
    QJsonObject adminRead(const QString& action, const QJsonObject& data) const;
    bool tick(const QDateTime& now, QString* diagnostic = nullptr) const;

private:
    QueueRepository* repository_ = nullptr;
    UtcClock clock_;
};
} // namespace charging::server
