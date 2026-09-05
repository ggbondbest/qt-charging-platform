// WalletService behaviour on the mock transport: profile fetch, recharge,
// record paging, duplicate-submission guards and error propagation.
// Since PR #21 the mock channel stays as the connection-less preview backend,
// so these tests double as contract-v1 parity checks: wherever the mock
// answers, the live server must answer the same way (docs/api/user_api_contract.md).

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

// Raw request straight into the mock transport, bypassing the service-layer
// guards — used for contract-v1 parity checks on validation/error semantics.
struct TransportReply
{
    bool done = false;
    bool ok = false;
    QJsonObject data;
    charging::protocol::ProtocolError error;
};

bool sendAndWait(MockRequestTransport& transport, const QString& type, const QJsonObject& data,
                 TransportReply* reply, int timeoutMs = kWaitMs)
{
    transport.send(type, data, [reply](bool ok, const QJsonObject& payload,
                                       const charging::protocol::ProtocolError& error) {
        reply->done = true;
        reply->ok = ok;
        reply->data = payload;
        reply->error = error;
    });
    for (int elapsed = 0; elapsed < timeoutMs && !reply->done; elapsed += 50) {
        QTest::qWait(50);
    }
    return reply->done;
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
        const auto error = failed.at(0).at(1).value<charging::protocol::ProtocolError>();
        QCOMPARE(error.code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidArgument));
        QCOMPARE(error.details.value(QStringLiteral("field")).toString(),
                 QStringLiteral("nickname")); // Same shape as the live server (§3).
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
    QCOMPARE(loaded.first().transactionNo.size(), 36); // Client-generated UUID, also used by Mock.
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
        service.recharge(WalletService::kMaximumRechargeCents + 1);
        QCOMPARE(failed.count(), 2); // Both rejected synchronously.
        for (int index = 0; index < failed.count(); ++index) {
            const auto error = failed.at(index).at(1).value<charging::protocol::ProtocolError>();
            QCOMPARE(error.code,
                     QString::fromLatin1(charging::protocol::error_code::kInvalidArgument));
            QCOMPARE(error.details.value(QStringLiteral("field")).toString(),
                     QStringLiteral("amountCents")); // Bounds frozen in §3.
        }
    }

    void pendingRechargeIntentOnlyExistsOnLiveChannel()
    {
        MockRequestTransport transport;
        WalletService service(&transport);
        // The mock scope is empty (preview), so no persisted intent is ever
        // reported and the recovery bar has nothing to show.
        QCOMPARE(service.pendingRechargeAmount(), qint64(0));
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

    void mockRechargeRecordsChainTimeOrderAndBalances()
    {
        MockRequestTransport transport;
        WalletService service(&transport);

        QSignalSpy records(&service, &WalletService::rechargeRecordsLoaded);
        service.fetchRechargeRecords(1);
        QVERIFY(waitForSignal(records));
        const auto loaded = qvariant_cast<QVector<charging::model::RechargeRecord>>(
            records.at(0).at(0));
        QCOMPARE(loaded.size(), 3);

        // 真实服务器自增 id 即时间序：id 倒序必须与时间倒序同向，否则列表
        // "最新在前"在 mock 上是假的（种子曾把最旧一笔排在了最上面）。
        for (int index = 1; index < loaded.size(); ++index) {
            QVERIFY2(loaded.at(index - 1).id > loaded.at(index).id, "id must be DESC");
            QVERIFY2(loaded.at(index - 1).createdAtUtc > loaded.at(index).createdAtUtc,
                     "createdAt must be DESC");
        }
        // 快照链闭合：较新一笔的快照减去它自己的金额 = 相邻较旧一笔的快照，
        // 且最新一笔的快照恰等于当前余额（否则页面两处数字对不上）。
        for (int index = 1; index < loaded.size(); ++index) {
            QCOMPARE(loaded.at(index - 1).balanceAfterCents - loaded.at(index - 1).amountCents,
                     loaded.at(index).balanceAfterCents);
        }
        QSignalSpy profile(&service, &WalletService::profileLoaded);
        service.fetchProfile();
        QVERIFY(waitForSignal(profile));
        QCOMPARE(loaded.first().balanceAfterCents,
                 qvariant_cast<charging::model::User>(profile.at(0).at(0)).balanceCents);
    }

    // ---- contract-v1 parity (docs/api/user_api_contract.md §2/§3) ----

    void mockAcceptsEveryFrozenAvatarKeyAndRejectsUnknown()
    {
        MockRequestTransport transport;
        const QStringList frozen{QStringLiteral("bolt"), QStringLiteral("plug"),
                                 QStringLiteral("car"), QStringLiteral("leaf"),
                                 QStringLiteral("cat"), QStringLiteral("panda"),
                                 QStringLiteral("moon"), QStringLiteral("rocket")};
        for (const QString& key : frozen) {
            TransportReply reply;
            QVERIFY(sendAndWait(transport,
                                QString::fromLatin1(
                                    charging::protocol::request_type::kUpdateUserInfo),
                                {{QStringLiteral("avatarKey"), key}}, &reply));
            QVERIFY2(reply.ok, qPrintable(QStringLiteral("reject ") + key + reply.error.code));
            QCOMPARE(reply.data.value(QStringLiteral("user")).toObject()
                         .value(QStringLiteral("avatarKey")).toString(), key);
        }
        const QStringList bad{QStringLiteral("not-a-key"), QStringLiteral("bolt/../x"),
                              QStringLiteral("有 emoji 🚀"),
                              QString(65, QChar(u'a'))};
        for (const QString& key : bad) {
            TransportReply reply;
            QVERIFY(sendAndWait(transport,
                                QString::fromLatin1(
                                    charging::protocol::request_type::kUpdateUserInfo),
                                {{QStringLiteral("avatarKey"), key}}, &reply));
            QVERIFY2(!reply.ok, qPrintable(QStringLiteral("accepted ") + key));
            QCOMPARE(reply.error.code,
                     QString::fromLatin1(charging::protocol::error_code::kInvalidArgument));
            QCOMPARE(reply.error.details.value(QStringLiteral("field")).toString(),
                     QStringLiteral("avatarKey"));
        }
    }

    void mockRechargeRejectsMissingTransactionNoAndOversizedAmount()
    {
        MockRequestTransport transport;
        const QString rechargeType =
            QString::fromLatin1(charging::protocol::request_type::kRecharge);

        TransportReply missing;
        QVERIFY(sendAndWait(transport, rechargeType, {{QStringLiteral("amountCents"), 5000}},
                            &missing));
        QVERIFY(!missing.ok);
        QCOMPARE(missing.error.code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidArgument));
        QCOMPARE(missing.error.details.value(QStringLiteral("field")).toString(),
                 QStringLiteral("transactionNo"));

        TransportReply oversize;
        QVERIFY(sendAndWait(transport, rechargeType,
                            {{QStringLiteral("amountCents"), 10000001},
                             {QStringLiteral("transactionNo"), QStringLiteral("T-OVER")}},
                            &oversize));
        QVERIFY(!oversize.ok);
        QCOMPARE(oversize.error.details.value(QStringLiteral("field")).toString(),
                 QStringLiteral("amountCents")); // §3: max 100000 yuan inclusive.
    }

    void mockRechargeReplayKeepsBothBalanceSemantics()
    {
        MockRequestTransport transport;
        const QString rechargeType =
            QString::fromLatin1(charging::protocol::request_type::kRecharge);

        TransportReply first;
        QVERIFY(sendAndWait(transport, rechargeType,
                            {{QStringLiteral("amountCents"), 1000},
                             {QStringLiteral("transactionNo"), QStringLiteral("TX-AAA")}},
                            &first));
        QVERIFY(first.ok);
        QCOMPARE(first.data.value(QStringLiteral("idempotent")).toBool(), false);
        QCOMPARE(first.data.value(QStringLiteral("balanceCents")).toDouble(), qint64(11000));

        // Another confirmed recharge moves the current balance onward.
        TransportReply second;
        QVERIFY(sendAndWait(transport, rechargeType,
                            {{QStringLiteral("amountCents"), 2000},
                             {QStringLiteral("transactionNo"), QStringLiteral("TX-BBB")}},
                            &second));
        QVERIFY(second.ok);

        // Replaying the original intent: same record snapshot, current balance,
        // no new credit (§3 + §5 "两种余额" acceptance item).
        TransportReply replay;
        QVERIFY(sendAndWait(transport, rechargeType,
                            {{QStringLiteral("amountCents"), 1000},
                             {QStringLiteral("transactionNo"), QStringLiteral("TX-AAA")}},
                            &replay));
        QVERIFY(replay.ok);
        QCOMPARE(replay.data.value(QStringLiteral("idempotent")).toBool(), true);
        QCOMPARE(replay.data.value(QStringLiteral("balanceCents")).toDouble(), qint64(13000));
        const QJsonObject record = replay.data.value(QStringLiteral("record")).toObject();
        QCOMPARE(record.value(QStringLiteral("balanceAfterCents")).toDouble(), qint64(11000));
        QCOMPARE(record.value(QStringLiteral("amountCents")).toDouble(), qint64(1000));

        // Same number, different amount: conflict without leaking the record.
        TransportReply conflict;
        QVERIFY(sendAndWait(transport, rechargeType,
                            {{QStringLiteral("amountCents"), 2000},
                             {QStringLiteral("transactionNo"), QStringLiteral("TX-AAA")}},
                            &conflict));
        QVERIFY(!conflict.ok);
        QCOMPARE(conflict.error.code,
                 QString::fromLatin1(charging::protocol::error_code::kIdempotencyConflict));
        QVERIFY(conflict.data.isEmpty());
    }

    void mockRejectsInvalidPagination()
    {
        MockRequestTransport transport;
        const QString recordsType =
            QString::fromLatin1(charging::protocol::request_type::kGetRechargeRecords);

        TransportReply zeroPage;
        QVERIFY(sendAndWait(transport, recordsType, {{QStringLiteral("page"), 0}}, &zeroPage));
        QVERIFY(!zeroPage.ok);
        QCOMPARE(zeroPage.error.code,
                 QString::fromLatin1(charging::protocol::error_code::kInvalidArgument));
        QCOMPARE(zeroPage.error.details.value(QStringLiteral("field")).toString(),
                 QStringLiteral("page"));

        TransportReply hugeSize;
        QVERIFY(sendAndWait(transport, recordsType, {{QStringLiteral("pageSize"), 101}},
                            &hugeSize));
        QVERIFY(!hugeSize.ok);
        QCOMPARE(hugeSize.error.details.value(QStringLiteral("field")).toString(),
                 QStringLiteral("pageSize"));

        // Defaults come from the contract: page 1, pageSize 20.
        TransportReply defaults;
        QVERIFY(sendAndWait(transport, recordsType, QJsonObject{}, &defaults));
        QVERIFY(defaults.ok);
        QCOMPARE(defaults.data.value(QStringLiteral("page")).toInt(), 1);
        QCOMPARE(defaults.data.value(QStringLiteral("pageSize")).toInt(), 20);
        QCOMPARE(defaults.data.value(QStringLiteral("total")).toInt(), 3);
    }
};

QTEST_MAIN(TestWalletService)
#include "tst_wallet_service.moc"
