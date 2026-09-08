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

} // namespace charging::client
