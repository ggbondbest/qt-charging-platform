#include "charging/client/profile_charging/point_service.h"

#include "charging/common/protocol/protocol.h"

#include <QJsonArray>

namespace charging::client {
namespace {
QString checkInType()
{
    return QString::fromLatin1(charging::protocol::request_type::kCheckIn);
}
QString getPointsType()
{
    return QString::fromLatin1(charging::protocol::request_type::kGetPoints);
}
QString creditLevelRewardType()
{
    return QString::fromLatin1(charging::protocol::request_type::kCreditLevelReward);
}
} // namespace

PointService::PointService(IRequestTransport* transport, QObject* parent)
    : QObject(parent), transport_(transport)
{
}

bool PointService::isBusy() const
{
    return busy_;
}

void PointService::fetchPoints(int page, int pageSize)
{
    if (transport_ == nullptr || busy_) {
        return;   // 单飞：静默丢弃并发请求（StatsService 同款口径）
    }
    busy_ = true;
    const QString type = getPointsType();
    transport_->sendFor(this, type, {{QStringLiteral("page"), page},
                            {QStringLiteral("pageSize"), pageSize}},
                     [this, type](bool ok, const QJsonObject& data,
                                  const charging::protocol::ProtocolError& error) {
                         busy_ = false;
                         if (!ok) {
                             emit operationFailed(type, error);
                             return;
                         }
                         emit pointsLoaded(
                             static_cast<qint64>(data.value(QStringLiteral("points")).toDouble()),
                             data.value(QStringLiteral("entries")).toArray().toVariantList(),
                             data.value(QStringLiteral("total")).toInt());
                     });
}

void PointService::checkIn()
{
    if (transport_ == nullptr || busy_) {
        return;
    }
    busy_ = true;
    const QString type = checkInType();
    transport_->sendFor(this, type, QJsonObject{},
                     [this, type](bool ok, const QJsonObject& data,
                                  const charging::protocol::ProtocolError& error) {
                         busy_ = false;
                         if (!ok) {
                             emit operationFailed(type, error);
                             return;
                         }
                         emit checkInCompleted(
                             data.value(QStringLiteral("day")).toString(),
                             static_cast<qint64>(data.value(QStringLiteral("points")).toDouble()),
                             static_cast<qint64>(data.value(QStringLiteral("gained")).toDouble()),
                             data.value(QStringLiteral("alreadyCheckedIn")).toBool());
                     });
}

void PointService::creditLevelReward(int level)
{
    if (transport_ == nullptr) {
        return;
    }
    // 刻意不查 busy_（见头注）：升级入账与流水刷新并发安全，服务端流水去重幂等。
    const QString type = creditLevelRewardType();
    transport_->sendFor(this, type, {{QStringLiteral("level"), level}},
                     [this, type, level](bool ok, const QJsonObject& data,
                                  const charging::protocol::ProtocolError& error) {
                         if (!ok) {
                             emit operationFailed(type, error);
                             return;
                         }
                         emit levelRewardCredited(
                             level,
                             static_cast<qint64>(data.value(QStringLiteral("points")).toDouble()),
                             static_cast<qint64>(data.value(QStringLiteral("gained")).toDouble()),
                             data.value(QStringLiteral("alreadyCredited")).toBool());
                     });
}

} // namespace charging::client
