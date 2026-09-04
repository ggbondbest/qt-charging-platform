// WalletService behaviour on the mock transport: profile fetch, recharge,
// record paging, duplicate-submission guards and error propagation.
// This exercises the service boundary that pages rely on; it intentionally
// depends on MockRequestTransport (delete together when real interfaces land).

#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/protocol/protocol.h"

#include <QCoreApplication>
#include <QObject>
#include <QSignalSpy>
#include <QTest>

Q_DECLARE_METATYPE(charging::model::User)
Q_DECLARE_METATYPE(charging::model::RechargeRecord)
Q_DECLARE_METATYPE(QVector<charging::model::RechargeRecord>)
Q_DECLARE_METATYPE(charging::protocol::ProtocolError)

namespace {

using charging::client::MockRequestTransport;
using charging::client::WalletService;

constexpr int kWaitMs = 3000;

void registerMetaTypes()
{
    qRegisterMetaType<charging::model::User>("charging::model::User");
    qRegisterMetaType<QVector<charging::model::RechargeRecord>>(
        "QVector<charging::model::RechargeRecord>");
    qRegisterMetaType<charging::protocol::ProtocolError>("charging::protocol::ProtocolError");
}

bool waitForSignal(QSignalSpy& spy, int timeoutMs = kWaitMs)
{
    // Qt 6.2's QSignalSpy::wait() does not cooperate with custom metatype
    // arguments, so tests poll the spy instead.
    for (int elapsed = 0; elapsed < timeoutMs && spy.isEmpty(); elapsed += 50) {
        QTest::qWait(50);
    }
    return !spy.isEmpty();
}

} // namespace

class TestWalletService final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Callbacks posted by QTimer::singleShot need an event loop.
        QVERIFY(QCoreApplication::instance() != nullptr);
        registerMetaTypes();
    }

    void fetchProfileReturnsDemoUser()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.fetchProfile();
        QVERIFY(waitForSignal(loaded));

        const charging::model::User user = qvariant_cast<charging::model::User>(loaded.at(0).at(0));
        QCOMPARE(user.phone, QStringLiteral("13800138000"));
        QCOMPARE(user.balanceCents, qint64(10000));
    }

    void updateNicknameRoundTripsAndTrims()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.updateNickname(QStringLiteral("  新昵称  "));
        QVERIFY(waitForSignal(loaded));

        const charging::model::User updated =
            qvariant_cast<charging::model::User>(loaded.at(0).at(0));
        QCOMPARE(updated.nickname, QStringLiteral("新昵称"));
        QVERIFY(!service.isUpdatingNickname());

        // The mock keeps the change: a fresh fetch reports the same nickname.
        QSignalSpy refetched(&service, &WalletService::profileLoaded);
        service.fetchProfile();
        QVERIFY(waitForSignal(refetched));
        const charging::model::User reread =
            qvariant_cast<charging::model::User>(refetched.at(0).at(0));
        QCOMPARE(reread.nickname, QStringLiteral("新昵称"));
    }

    void invalidNicknamesFailLocallyWithoutRoundTrip()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy failed(&service, &WalletService::operationFailed);
        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.updateNickname(QStringLiteral("   "));
        service.updateNickname(QString(33, QChar(u'名')));
        QCOMPARE(failed.count(), 2); // Both rejected synchronously.
        QCOMPARE(loaded.count(), 0);
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope));
        QVERIFY(!service.isUpdatingNickname());
    }

    void duplicateUpdateNicknameIsIgnoredWhileInFlight()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.updateNickname(QStringLiteral("第一个"));
        service.updateNickname(QStringLiteral("第二个")); // Swallowed by the guard.
        QVERIFY(waitForSignal(loaded));
        QTest::qWait(600);
        QCOMPARE(loaded.count(), 1);
        const charging::model::User updated =
            qvariant_cast<charging::model::User>(loaded.at(0).at(0));
        QCOMPARE(updated.nickname, QStringLiteral("第一个"));
    }

    void updateAvatarRoundTripsAndDefaultRestores()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.updateAvatar(QStringLiteral("bolt"));
        QVERIFY(waitForSignal(loaded));

        const charging::model::User updated =
            qvariant_cast<charging::model::User>(loaded.at(0).at(0));
        QCOMPARE(updated.avatarKey, QStringLiteral("bolt"));
        QVERIFY(!service.isUpdatingAvatar());

        // The mock keeps the choice; "" restores the default (initial) avatar.
        QSignalSpy reset(&service, &WalletService::profileLoaded);
        service.updateAvatar(QString());
        QVERIFY(waitForSignal(reset));
        const charging::model::User restored =
            qvariant_cast<charging::model::User>(reset.at(0).at(0));
        QVERIFY(restored.avatarKey.isEmpty());
    }

    void duplicateUpdateAvatarIsIgnoredWhileInFlight()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.updateAvatar(QStringLiteral("cat"));
        service.updateAvatar(QStringLiteral("moon")); // Swallowed by the guard.
        QVERIFY(waitForSignal(loaded));
        QTest::qWait(600);
        QCOMPARE(loaded.count(), 1);
        QCOMPARE(qvariant_cast<charging::model::User>(loaded.at(0).at(0)).avatarKey,
                 QStringLiteral("cat"));
    }

    void updateNicknameFailurePropagatesAndReleasesGuard()    {
        MockRequestTransport transport;
        WalletService service(&transport);

        transport.setNextFailure(
            QString::fromLatin1(charging::protocol::error_code::kConnectionError));

        QSignalSpy failed(&service, &WalletService::operationFailed);
        service.updateNickname(QStringLiteral("改名"));
        QVERIFY(waitForSignal(failed));
        QCOMPARE(failed.at(0).at(0).toString(),
                 QString::fromLatin1(charging::protocol::request_type::kUpdateUserInfo));
        QVERIFY(!service.isUpdatingNickname());

        // A retry after the transient failure must succeed.
        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.updateNickname(QStringLiteral("改名"));
        QVERIFY(waitForSignal(loaded));
    }

    void rechargeUpdatesBalanceAndCreatesRecord()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy completed(&service, &WalletService::rechargeCompleted);
        service.recharge(5000);
        QVERIFY(waitForSignal(completed));
        QCOMPARE(qint64(completed.at(0).at(0).toLongLong()), qint64(5000));
        QCOMPARE(qint64(completed.at(0).at(1).toLongLong()), qint64(15000));

        QSignalSpy records(&service, &WalletService::rechargeRecordsLoaded);
        service.fetchRechargeRecords(1);
        QVERIFY(waitForSignal(records));

        const auto loaded = qvariant_cast<QVector<charging::model::RechargeRecord>>(
            records.at(0).at(0));
        QVERIFY(!loaded.isEmpty());
        QCOMPARE(loaded.first().amountCents, qint64(5000));
        QCOMPARE(loaded.first().balanceAfterCents, qint64(15000));
        QVERIFY(loaded.first().transactionNo.startsWith(QStringLiteral("MOCKRCH")));
    }

    void duplicateRechargeIsIgnoredWhileInFlight()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy completed(&service, &WalletService::rechargeCompleted);
        service.recharge(1000);
        service.recharge(1000); // Must be swallowed by the in-flight guard.
        QVERIFY(waitForSignal(completed));

        QVERIFY(!service.isRecharging());
        QTest::qWait(600); // Well past one mock latency window.
        QCOMPARE(completed.count(), 1);
    }

    void invalidAmountFailsLocallyWithoutTransportRoundTrip()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy failed(&service, &WalletService::operationFailed);
        service.recharge(0);
        QCOMPARE(failed.count(), 1); // Rejected synchronously.
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope));
    }

    void transportFailurePropagatesErrorCode()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        transport.setNextFailure(
            QString::fromLatin1(charging::protocol::error_code::kConnectionError));

        QSignalSpy failed(&service, &WalletService::operationFailed);
        service.fetchProfile();
        QVERIFY(waitForSignal(failed));
        QCOMPARE(failed.at(0).at(1).value<charging::protocol::ProtocolError>().code,
                 QString::fromLatin1(charging::protocol::error_code::kConnectionError));

        // The guard must have been released so a retry can proceed.
        QVERIFY(!service.isFetchingProfile());
        QSignalSpy loaded(&service, &WalletService::profileLoaded);
        service.fetchProfile();
        QVERIFY(waitForSignal(loaded));
    }

    void recordPaginationReportsHasMore()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        // Seed has 3 records; pageSize is 10 so a single page answers all.
        QSignalSpy records(&service, &WalletService::rechargeRecordsLoaded);
        service.fetchRechargeRecords(1);
        QVERIFY(waitForSignal(records));
        const bool hasMore = records.at(0).at(2).toBool();
        QCOMPARE(hasMore, false);
        const int total = records.at(0).at(1).toInt();
        QCOMPARE(total, 3);
    }
};

QTEST_MAIN(TestWalletService)
#include "tst_wallet_service.moc"
