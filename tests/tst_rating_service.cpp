// RatingService behaviour on the mock transport (2026-09-08 批次E): seeded
// demo rating shape, one-rating-per-order idempotent replay, NOT_FOUND for
// unratable orders, single-flight guard and error propagation. Real-server
// semantics (order snapshot, paging SQL) are pinned in tst_user_api_integration.

#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/rating_service.h"
#include "charging/common/protocol/protocol.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSignalSpy>
#include <QTest>

Q_DECLARE_METATYPE(charging::protocol::ProtocolError)

namespace {

using charging::client::MockRequestTransport;
using charging::client::RatingService;

constexpr int kWaitMs = 3000;

void registerMetaTypes()
{
    qRegisterMetaType<charging::protocol::ProtocolError>("charging::protocol::ProtocolError");
}

bool waitForSignal(QSignalSpy& spy, int timeoutMs = kWaitMs)
{
    // Qt 6.2's QSignalSpy::wait() does not cooperate with custom metatype
    // arguments, so tests poll the spy instead (wallet 同款)。
    for (int elapsed = 0; elapsed < timeoutMs && spy.isEmpty(); elapsed += 50) {
        QTest::qWait(50);
    }
    return !spy.isEmpty();
}

} // namespace

class RatingServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { registerMetaTypes(); }

    void seedSitsOnNewestCompletedOrder();
    void submitGrantsOnceThenReplays();
    void unratableOrdersFailWithNotFound();
    void singleFlightDropsConcurrentRequests();
    void transportFailureSurfacesOperationFailed();
};

void RatingServiceTest::seedSitsOnNewestCompletedOrder()
{
    MockRequestTransport transport;
    RatingService service(&transport);
    QSignalSpy loaded(&service, &RatingService::ratingsLoaded);
    service.fetchMyRatings();
    QVERIFY(waitForSignal(loaded));
    const QVariantList args = loaded.takeFirst();
    const QVariantList rows = args.at(0).toList();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(args.at(1).toInt(), 1);
    // 演示种子挂在最近一笔已完成单（mock 订单 id=3，桩 12 云杉科技园区 B07）。
    const QVariantMap row = rows.first().toMap();
    QCOMPARE(row.value("orderId").toString(), QStringLiteral("3"));
    QCOMPARE(row.value("chargerId").toString(), QStringLiteral("12"));
    QCOMPARE(row.value("chargerCode").toString(), QStringLiteral("B07"));
    QCOMPARE(row.value("stationName").toString(), QStringLiteral("云杉科技园区充电站"));
    QCOMPARE(row.value("rating").toInt(), 5);
    QCOMPARE(row.value("comment").toString(), QStringLiteral("充电很快，环境不错。"));
    QVERIFY(row.value("id").toString().isEmpty() == false);
    QVERIFY(row.value("createdAtUtc").toString().size() > 10);
}

void RatingServiceTest::submitGrantsOnceThenReplays()
{
    MockRequestTransport transport;
    RatingService service(&transport);

    // 订单 id=2（Completed、未评价）首评：comment trim 后入库、行 prepend。
    QSignalSpy done(&service, &RatingService::ratingSubmitted);
    service.submitRating(QStringLiteral("2"), 4, QStringLiteral("  很快  "));
    QVERIFY(waitForSignal(done));
    QVariantList args = done.takeFirst();
    QVariantMap row = args.at(0).toMap();
    QCOMPARE(row.value("orderId").toString(), QStringLiteral("2"));
    QCOMPARE(row.value("chargerId").toString(), QStringLiteral("11"));
    QCOMPARE(row.value("rating").toInt(), 4);
    QCOMPARE(row.value("comment").toString(), QStringLiteral("很快"));
    QVERIFY(row.value("createdAtUtc").toString().isEmpty() == false);
    QCOMPARE(args.at(1).toBool(), false);

    // 重放：幂等成功、alreadyRated=true、返回首评原值（不覆盖）。
    service.submitRating(QStringLiteral("2"), 1, QStringLiteral("改了"));
    QVERIFY(waitForSignal(done));
    args = done.takeFirst();
    QCOMPARE(args.at(1).toBool(), true);
    QCOMPARE(args.at(0).toMap().value("rating").toInt(), 4);
    QCOMPARE(args.at(0).toMap().value("comment").toString(), QStringLiteral("很快"));

    // 列表页：两行、新在前（id=2 晚于种子 id=3 入库 → prepend 到队首）。
    QSignalSpy loaded(&service, &RatingService::ratingsLoaded);
    service.fetchMyRatings();
    QVERIFY(waitForSignal(loaded));
    const QVariantList rows = loaded.takeFirst().at(0).toList();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.first().toMap().value("orderId").toString(), QStringLiteral("2"));

    // normalize 域：非法星级在 mock 也走契约校验。
    QSignalSpy failed(&service, &RatingService::operationFailed);
    service.submitRating(QStringLiteral("2"), 6, QString());
    QVERIFY(waitForSignal(failed));
    QCOMPARE(failed.takeFirst().at(0).toString(), QStringLiteral("SUBMIT_CHARGER_RATING"));
}

void RatingServiceTest::unratableOrdersFailWithNotFound()
{
    MockRequestTransport transport;
    RatingService service(&transport);
    QSignalSpy failed(&service, &RatingService::operationFailed);
    service.submitRating(QStringLiteral("4"), 5, QString());   // WaitingPayment
    QVERIFY(waitForSignal(failed));
    QCOMPARE(failed.takeFirst().at(0).toString(), QStringLiteral("SUBMIT_CHARGER_RATING"));
    service.submitRating(QStringLiteral("999"), 5, QString()); // 不存在
    QVERIFY(waitForSignal(failed));
    QVERIFY(!service.isBusy());
}

void RatingServiceTest::singleFlightDropsConcurrentRequests()
{
    MockRequestTransport transport;
    RatingService service(&transport);
    QVERIFY(!service.isBusy());
    service.fetchMyRatings();
    QVERIFY(service.isBusy());
    service.submitRating(QStringLiteral("2"), 3, QString());   // busy 中被静默丢弃
    QSignalSpy done(&service, &RatingService::ratingSubmitted);
    QSignalSpy loaded(&service, &RatingService::ratingsLoaded);
    QVERIFY(waitForSignal(loaded));
    QCOMPARE(loaded.count(), 1);
    QTest::qWait(600);                              // 越过 mock 延迟：不该有提交
    QCOMPARE(done.count(), 0);
    QVERIFY(!service.isBusy());
}

void RatingServiceTest::transportFailureSurfacesOperationFailed()
{
    MockRequestTransport transport;
    RatingService service(&transport);
    transport.setNextFailure(QStringLiteral("INTERNAL_ERROR"));
    QSignalSpy failed(&service, &RatingService::operationFailed);
    service.fetchMyRatings();
    QVERIFY(waitForSignal(failed));
    const QVariantList args = failed.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("GET_MY_RATINGS"));
    QVERIFY(!service.isBusy());                     // 失败必须解锁单飞
}

QTEST_GUILESS_MAIN(RatingServiceTest)
#include "tst_rating_service.moc"
