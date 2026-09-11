#pragma once

#include "charging/client/profile_charging/i_request_transport.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace charging::client {

// 电桩评价（2026-09-08 批次E）。Wire actions: SUBMIT_CHARGER_RATING（写型，
// 一单一评幂等——服务端 order_id UNIQUE 即锁，重放不覆盖首评）、
// GET_MY_RATINGS（分页读本人评价流水）。ratings 行 = GET_MY_RATINGS 响应形：
// [{id, orderId, chargerId, chargerCode, stationName, rating, comment,
// createdAtUtc}] 新→旧。TODO(contract): 改评/删评能力二期再议。
class RatingService final : public QObject
{
    Q_OBJECT

public:
    explicit RatingService(IRequestTransport* transport, QObject* parent = nullptr);

    bool isBusy() const;              // 两请求共用的单飞旗标（PointService 同款）
    void fetchMyRatings(int page = 1, int pageSize = 20);
    // 提交成功/幂等重放都发 ratingSubmitted；ratingRow 为服务端回读行
    // （重放=首评原值），页面据此直接进入"已评价"态。
    void submitRating(const QString& orderId, int rating, const QString& comment);

signals:
    void ratingsLoaded(const QVariantList& ratings, int total);
    void ratingSubmitted(const QVariantMap& ratingRow, bool alreadyRated);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    IRequestTransport* transport_ = nullptr;
    bool busy_ = false;
};

} // namespace charging::client
