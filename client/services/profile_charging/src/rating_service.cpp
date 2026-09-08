#include "charging/client/profile_charging/rating_service.h"

#include "charging/common/protocol/protocol.h"

#include <QJsonArray>

namespace charging::client {
namespace {
QString submitType()
{
    return QString::fromLatin1(charging::protocol::request_type::kSubmitChargerRating);
}
QString listType()
{
    return QString::fromLatin1(charging::protocol::request_type::kGetMyRatings);
}
} // namespace

RatingService::RatingService(IRequestTransport* transport, QObject* parent)
    : QObject(parent), transport_(transport)
{
}

bool RatingService::isBusy() const
{
    return busy_;
}

void RatingService::fetchMyRatings(int page, int pageSize)
{
    if (transport_ == nullptr || busy_) {
        return;   // 单飞：静默丢弃并发请求（PointService 同款口径）
    }
    busy_ = true;
    const QString type = listType();
    transport_->send(type, {{QStringLiteral("page"), page},
                            {QStringLiteral("pageSize"), pageSize}},
                     [this, type](bool ok, const QJsonObject& data,
                                  const charging::protocol::ProtocolError& error) {
                         busy_ = false;
                         if (!ok) {
                             emit operationFailed(type, error);
                             return;
                         }
                         emit ratingsLoaded(
                             data.value(QStringLiteral("ratings")).toArray().toVariantList(),
                             data.value(QStringLiteral("total")).toInt());
                     });
}

void RatingService::submitRating(const QString& orderId, int rating, const QString& comment)
{
    if (transport_ == nullptr || busy_) {
        return;
    }
    busy_ = true;
    const QString type = submitType();
    // orderId 必须正十进制串（服务端 normalize 形态校验）；rating 1..5。
    transport_->send(type, {{QStringLiteral("orderId"), orderId},
                            {QStringLiteral("rating"), rating},
                            {QStringLiteral("comment"), comment.trimmed()}},
                     [this, type](bool ok, const QJsonObject& data,
                                  const charging::protocol::ProtocolError& error) {
                         busy_ = false;
                         if (!ok) {
                             emit operationFailed(type, error);
                             return;
                         }
                         emit ratingSubmitted(
                             data.value(QStringLiteral("rating")).toObject().toVariantMap(),
                             data.value(QStringLiteral("alreadyRated")).toBool());
                     });
}

} // namespace charging::client
