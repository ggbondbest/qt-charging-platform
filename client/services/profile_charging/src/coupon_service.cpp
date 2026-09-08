#include "charging/client/profile_charging/coupon_service.h"

#include "charging/common/protocol/protocol.h"

#include <QJsonArray>

namespace charging::client {

CouponService::CouponService(IRequestTransport* transport, QObject* parent)
    : QObject(parent), transport_(transport)
{
}

QVariantList CouponService::coupons() const
{
    return coupons_;
}

bool CouponService::isFetchingCoupons() const
{
    return fetching_;
}

void CouponService::fetchCoupons()
{
    if (transport_ == nullptr || fetching_) {
        return;
    }
    fetching_ = true;
    const QString type =
        QString::fromLatin1(charging::protocol::request_type::kGetCoupons);
    transport_->sendFor(this, type, {{QStringLiteral("pageSize"), kCouponsPageSize}},
                     [this, type](bool ok, const QJsonObject& data,
                                  const charging::protocol::ProtocolError& error) {
                         fetching_ = false;
                         if (!ok) {
                             emit operationFailed(type, error);
                             return;
                         }
                         coupons_ =
                             data.value(QStringLiteral("coupons")).toArray().toVariantList();
                         emit couponsChanged();
                     });
}

} // namespace charging::client
