// PointService behaviour on the mock transport (2026-09-08 批次C): daily
// check-in idempotency, ledger paging shape, single-flight guard and error
// propagation. Cross-UTC-day behaviour is proven against the real server in
// tst_user_api_integration (the mock clock is wall-clock); here we pin the
// same-day replay semantics the mock mirrors.

#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/point_service.h"
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
using charging::client::PointService;

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

class PointServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { registerMetaTypes(); }

    void initialLedgerCarriesWelcomeSeed();
    void checkInGrantsOnceThenReplays();
    void singleFlightDropsConcurrentRequests();
    void transportFailureSurfacesOperationFailed();
};

void PointServiceTest::initialLedgerCarriesWelcomeSeed()
{
    MockRequestTransport transport;
    PointService service(&transport);
    QSignalSpy loaded(&service, &PointService::pointsLoaded);
    service.fetchPoints();
    QVERIFY(waitForSignal(loaded));
    const QVariantList args = loaded.takeFirst();
    QCOMPARE(args.at(0).toLongLong(), 50);          // 注册礼包 seed
    QCOMPARE(args.at(2).toInt(), 1);
    const QVariantList entries = args.at(1).toList();
    QCOMPARE(entries.size(), 1);
    const QVariantMap entry = entries.first().toMap();
    QCOMPARE(entry.value("amount").toLongLong(), 50);
    QCOMPARE(entry.value("reason").toString(), QStringLiteral("注册礼包"));
    QVERIFY(entry.value("id").toString().isEmpty() == false);
    QVERIFY(entry.value("createdAtUtc").toString().size() > 10);
}

void PointServiceTest::checkInGrantsOnceThenReplays()
{
    MockRequestTransport transport;
    PointService service(&transport);

    QSignalSpy done(&service, &PointService::checkInCompleted);
    service.checkIn();
    QVERIFY(waitForSignal(done));
    QVariantList args = done.takeFirst();
    QCOMPARE(args.at(1).toLongLong(), 60);          // 50 seed + 10
    QCOMPARE(args.at(2).toLongLong(), 10);
    QCOMPARE(args.at(3).toBool(), false);
    QCOMPARE(args.at(0).toString().size(), 10);     // "YYYY-MM-DD"

    // 同日重放：幂等成功、gained=0、总分不变（服务端 user_checkins 同款语义）。
    service.checkIn();
    QVERIFY(waitForSignal(done));
    args = done.takeFirst();
    QCOMPARE(args.at(1).toLongLong(), 60);
    QCOMPARE(args.at(2).toLongLong(), 0);
    QCOMPARE(args.at(3).toBool(), true);

    // 流水页看到入账行在最前（新→旧）。
    QSignalSpy loaded(&service, &PointService::pointsLoaded);
    service.fetchPoints();
    QVERIFY(waitForSignal(loaded));
    const QVariantList entries = loaded.takeFirst().at(1).toList();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.first().toMap().value("reason").toString(), QStringLiteral("每日签到"));
}

void PointServiceTest::singleFlightDropsConcurrentRequests()
{
    MockRequestTransport transport;
    PointService service(&transport);
    QVERIFY(!service.isBusy());
    service.checkIn();
    QVERIFY(service.isBusy());
    service.fetchPoints();                          // busy 中被静默丢弃
    QSignalSpy loaded(&service, &PointService::pointsLoaded);
    QSignalSpy done(&service, &PointService::checkInCompleted);
    QVERIFY(waitForSignal(done));
    QCOMPARE(done.count(), 1);
    QTest::qWait(600);                              // 越过 mock 延迟：不该有 pointsLoaded
    QCOMPARE(loaded.count(), 0);
    QVERIFY(!service.isBusy());
}

void PointServiceTest::transportFailureSurfacesOperationFailed()
{
    MockRequestTransport transport;
    PointService service(&transport);
    transport.setNextFailure(QStringLiteral("INTERNAL_ERROR"));
    QSignalSpy failed(&service, &PointService::operationFailed);
    service.checkIn();
    QVERIFY(waitForSignal(failed));
    const QVariantList args = failed.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("CHECK_IN"));
    QVERIFY(!service.isBusy());                     // 失败必须解锁单飞
}

QTEST_GUILESS_MAIN(PointServiceTest)
#include "tst_point_service.moc"
