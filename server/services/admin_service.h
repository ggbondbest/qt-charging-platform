#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>

namespace charging::server {
class AdminRepository;
class QueueService;
class RepairService;

// Worker-thread-only service. Tokens are local management capabilities and are
// deliberately NOT routed through the public user TCP RequestDispatcher.
class AdminService final
{
public:
    explicit AdminService(AdminRepository* repository, qint64 idleTimeoutMs = 30 * 60 * 1000);
    QJsonObject handle(const QString& action, const QJsonObject& data,
                       const QString& sessionToken = {}, const QString& channel = {});
    static QJsonObject failure(const QString& code);
    void setWorkflowServices(QueueService* queue, RepairService* repair)
    { queueService_ = queue; repairService_ = repair; }

private:
    QueueService* queueService_ = nullptr;
    RepairService* repairService_ = nullptr;
    struct Session
    {
        qint64 adminId;
        QString credentialStamp;
        qint64 touched;
        qint64 created;
        QString channel;
    };
    QJsonObject login(const QJsonObject& data, const QString& channel);
    AdminRepository* repository_;
    QElapsedTimer clock_;
    QHash<QString, Session> sessions_;
    qint64 idleTimeoutMs_;
    int loginFailures_ = 0;
    qint64 blockedUntil_ = 0;
};
} // namespace charging::server
