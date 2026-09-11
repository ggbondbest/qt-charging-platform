// Async-callback lifetime regression (PR review 2026-09-08, P1-1): the four
// user-domain services must bind their transport callbacks to the receiver via
// IRequestTransport::sendFor. A response arriving after the service was
// destroyed (logout/session swap/app quit with a request in flight) must be
// discarded, never dereference the dead object. Each case destroys the service
// while its request is still pending, waits past the mock latency, then proves
// the transport is still healthy by completing a request on a fresh service.

#include "charging/client/profile_charging/coupon_service.h"
#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/point_service.h"
#include "charging/client/profile_charging/rating_service.h"
#include "charging/client/profile_charging/stats_service.h"
#include "charging/common/protocol/protocol.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

Q_DECLARE_METATYPE(charging::protocol::ProtocolError)

namespace {

using charging::client::CouponService;
using charging::client::MockRequestTransport;
using charging::client::PointService;
using charging::client::RatingService;
using charging::client::StatsService;

// Mock round trips land ~450ms after send; outlive that window comfortably.
constexpr int kLateResponseWindowMs = 1500;

// Qt 6.2's QSignalSpy::wait() does not cooperate with custom metatype
// arguments, so tests poll the spy instead (wallet 同款).
bool waitForSignal(QSignalSpy& spy, int timeoutMs = kLateResponseWindowMs)
{
    for (int elapsed = 0; elapsed < timeoutMs && spy.isEmpty(); elapsed += 50) {
        QTest::qWait(50);
    }
    return !spy.isEmpty();
}

} // namespace

class ServiceLifecycleTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<charging::protocol::ProtocolError>(
            "charging::protocol::ProtocolError");
    }

    void statsDestroyedBeforeResponseIsDiscarded();
    void couponsDestroyedBeforeResponseIsDiscarded();
    void pointsDestroyedBeforeResponseIsDiscarded();
    void ratingsDestroyedBeforeResponseIsDiscarded();
};

void ServiceLifecycleTest::statsDestroyedBeforeResponseIsDiscarded()
{
    MockRequestTransport transport;

    {
        StatsService service(&transport);
        QSignalSpy loaded(&service, &StatsService::statsLoaded);
        service.fetchStats();
        QVERIFY(service.isFetchingStats());   // request genuinely in flight
    }                                     // service dies with the response pending

    QTest::qWait(kLateResponseWindowMs);      // late response must be discarded

    StatsService survivor(&transport);        // transport must still be usable
    QSignalSpy loaded(&survivor, &StatsService::statsLoaded);
    survivor.fetchStats();
    QVERIFY(waitForSignal(loaded));
}

void ServiceLifecycleTest::couponsDestroyedBeforeResponseIsDiscarded()
{
    MockRequestTransport transport;

    {
        CouponService service(&transport);
        QSignalSpy changed(&service, &CouponService::couponsChanged);
        service.fetchCoupons();
        QVERIFY(service.isFetchingCoupons());
    }

    QTest::qWait(kLateResponseWindowMs);

    CouponService survivor(&transport);
    QSignalSpy changed(&survivor, &CouponService::couponsChanged);
    survivor.fetchCoupons();
    QVERIFY(waitForSignal(changed));
    QVERIFY(!survivor.coupons().isEmpty());   // fresh service served normally
}

void ServiceLifecycleTest::pointsDestroyedBeforeResponseIsDiscarded()
{
    MockRequestTransport transport;

    {
        PointService service(&transport);
        QSignalSpy loaded(&service, &PointService::pointsLoaded);
        service.fetchPoints();
    }

    QTest::qWait(kLateResponseWindowMs);

    PointService survivor(&transport);
    QSignalSpy completed(&survivor, &PointService::checkInCompleted);
    survivor.checkIn();
    QVERIFY(waitForSignal(completed));
}

void ServiceLifecycleTest::ratingsDestroyedBeforeResponseIsDiscarded()
{
    MockRequestTransport transport;

    {
        RatingService service(&transport);
        QSignalSpy loaded(&service, &RatingService::ratingsLoaded);
        service.fetchMyRatings();
    }

    QTest::qWait(kLateResponseWindowMs);

    RatingService survivor(&transport);
    QSignalSpy loaded(&survivor, &RatingService::ratingsLoaded);
    survivor.fetchMyRatings();
    QVERIFY(waitForSignal(loaded));
}

QTEST_GUILESS_MAIN(ServiceLifecycleTest)
#include "tst_service_lifecycle.moc"
