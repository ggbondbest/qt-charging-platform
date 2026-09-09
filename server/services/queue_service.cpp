#include "queue_service.h"
#include "queue_repository.h"

#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace charging::server {
namespace {
QJsonObject fail(const QString& code, const QString& message)
{
    return {{"success", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
bool positiveId(const QJsonValue& value)
{
    if (!value.isString()) return false;
    static const QRegularExpression pattern(QStringLiteral("^[1-9][0-9]{0,18}$"));
    bool ok = false;
    const qint64 id = value.toString().toLongLong(&ok);
    return pattern.match(value.toString()).hasMatch() && ok && id > 0;
}
bool operationId(const QJsonValue& value)
{
    if (!value.isString()) return false;
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{8,64}$"));
    return pattern.match(value.toString()).hasMatch();
}
bool pageNumber(const QJsonObject& value, const QString& key, int maximum)
{
    if (!value.contains(key)) return true;
    const auto number = value.value(key);
    return number.isDouble() && std::isfinite(number.toDouble()) &&
           number.toDouble() == std::floor(number.toDouble()) &&
           number.toDouble() >= 1 && number.toDouble() <= maximum;
}
} // namespace

QueueService::QueueService(QueueRepository* repository, UtcClock clock)
    : repository_(repository), clock_(clock) {}

bool QueueService::handles(const QString& type)
{
    static const QSet<QString> actions{"QUEUE_JOIN", "QUEUE_GET_MINE", "QUEUE_LEAVE", "QUEUE_CONFIRM"};
    return actions.contains(type);
}

QJsonObject QueueService::handle(const QString& type, const QJsonObject& data, qint64 userId) const
{
    if (!handles(type)) return fail("UNKNOWN_REQUEST_TYPE", QStringLiteral("未知排队请求"));
    if (userId <= 0) return fail("UNAUTHORIZED", QStringLiteral("请先登录"));
    if (data.contains("userId")) return fail("INVALID_ARGUMENT", QStringLiteral("无需传入用户身份"));
    if (type != "QUEUE_GET_MINE" && !operationId(data.value("operationId")))
        return fail("INVALID_ARGUMENT", QStringLiteral("操作编号无效，请刷新后重试"));
    if (type == "QUEUE_JOIN" && !positiveId(data.value("chargerId")))
        return fail("INVALID_ARGUMENT", QStringLiteral("请选择具体电桩"));
    if ((type == "QUEUE_LEAVE" || type == "QUEUE_CONFIRM") && !positiveId(data.value("id")))
        return fail("INVALID_ARGUMENT", QStringLiteral("排队编号无效"));
    if (!repository_) return fail("INTERNAL_ERROR", QStringLiteral("排队服务尚未就绪"));
    return repository_->execute(type, data, userId,
                               clock_ ? clock_().toUTC() : QDateTime::currentDateTimeUtc());
}

QJsonObject QueueService::adminRead(const QString& action, const QJsonObject& data) const
{
    if (action != "queues.list") return fail("UNKNOWN_REQUEST_TYPE", QStringLiteral("未知排队查询"));
    if ((data.contains("stationId") && !positiveId(data.value("stationId"))) ||
        (data.contains("chargerId") && !positiveId(data.value("chargerId"))) ||
        !pageNumber(data, "page", 1000000) || !pageNumber(data, "pageSize", 100))
        return fail("INVALID_ARGUMENT", QStringLiteral("电站、电桩或分页参数无效"));
    static const QSet<QString> statuses{"ACTIVE", "WAITING", "CALLED", "CONFIRMED", "LEFT", "EXPIRED", "ALL"};
    if (data.contains("status") && !statuses.contains(data.value("status").toString()))
        return fail("INVALID_ARGUMENT", QStringLiteral("排队状态无效"));
    if (!repository_) return fail("INTERNAL_ERROR", QStringLiteral("排队服务尚未就绪"));
    return repository_->adminRead(data, clock_ ? clock_().toUTC() : QDateTime::currentDateTimeUtc());
}

bool QueueService::tick(const QDateTime& now, QString* diagnostic) const
{
    if (!repository_) {
        if (diagnostic) *diagnostic = QStringLiteral("Queue repository is unavailable");
        return false;
    }
    return repository_->tick(now.toUTC(), diagnostic);
}
} // namespace charging::server
