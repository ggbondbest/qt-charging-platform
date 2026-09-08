#pragma once

#include "charging/client/profile_charging/i_request_transport.h"

#include <QObject>
#include <QVariantList>

namespace charging::client {

// Coupon wallet for CouponPage (member 2's page, wired blind: it calls
// couponService.coupons() and listens for couponsChanged). Wire action:
// GET_COUPONS (contract addition 2026-09-08). Rows keep the contract shape
// verbatim — no database model exists for coupons:
// [{id,kind,title,valueCents?|discountTenths?,thresholdCents,condition,
//   expiresAtUtc(ms),status,source}] newest first; tab filtering is the
// page's job, so the service always fetches the full wallet (status "").
class CouponService final : public QObject
{
    Q_OBJECT

public:
    static constexpr int kCouponsPageSize = 100;   // full-wallet pull

    explicit CouponService(IRequestTransport* transport, QObject* parent = nullptr);

    QVariantList coupons() const;    // cached rows, newest first
    bool isFetchingCoupons() const;
    void fetchCoupons();

signals:
    void couponsChanged();
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    IRequestTransport* transport_ = nullptr;
    QVariantList coupons_;
    bool fetching_ = false;
};

} // namespace charging::client
