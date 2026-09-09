// ProgressService behaviour (2026-09-09 经验等级批): level curve boundaries,
// day-granular task idempotency, all-tasks bonus, per-tier level-up gifts,
// cross-day reset and QSettings persistence per phone. Pure client-side —
// no transport involved. QSettings domain is isolated to a temp dir so the
// suite never touches the developer's real config.

#include "charging/client/profile_charging/progress_service.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using charging::client::ProgressService;

namespace {
// 每用例独立手机号 = 独立 QSettings 分组，互不串档。
QString casePhone(const char* tag) { return QStringLiteral("1390000%1").arg(tag); }

void completeAllTasks(ProgressService& service)
{
    service.reportEvent(QStringLiteral("checkin"));
    service.reportEvent(QStringLiteral("search"));
    service.reportEvent(QStringLiteral("detail"));
    service.reportEvent(QStringLiteral("route"));
    service.reportEvent(QStringLiteral("stats"));
}
} // namespace

class ProgressServiceTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDir_;

private slots:
    void initTestCase()
    {
        QVERIFY(settingsDir_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatformTests"));
        QCoreApplication::setApplicationName(QStringLiteral("progress-service-test"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir_.path());
    }

    void startsAtBronzeWithEmptyProgress();
    void taskEventsAwardOncePerDay();
    void unknownEventsAreIgnored();
    void allTasksGrantOnceDailyBonus();
    void levelUpsEmitGiftsPerTier();
    void crossDayReset();
    void statePersistsPerPhoneAcrossInstances();
    void differentPhonesAreIsolated();
};

void ProgressServiceTest::startsAtBronzeWithEmptyProgress()
{
    ProgressService service(casePhone("0001"));
    QCOMPARE(service.xp(), qint64(0));
    QCOMPARE(service.level(), 1);
    QCOMPARE(service.tierName(), QString::fromUtf8("青铜会员"));
    QCOMPARE(service.tierGlyph(), QString::fromUtf8("🥉"));
    QCOMPARE(service.xpToNext(), qint64(60));
    QCOMPARE(service.xpSpan(), qint64(60));
    QCOMPARE(service.progress(), 0.0);
    QCOMPARE(service.nextTierName(), QString::fromUtf8("白银会员"));
    QCOMPARE(service.doneTaskCount(), 0);
    QVERIFY(!service.allTasksDone());
    QCOMPARE(service.tasks().size(), 5);
    QCOMPARE(service.tierTable().size(), 5);
    QVERIFY(service.gifts().isEmpty());
}

void ProgressServiceTest::taskEventsAwardOncePerDay()
{
    ProgressService service(casePhone("0002"));
    QSignalSpy xpSpy(&service, &ProgressService::xpChanged);
    service.reportEvent(QStringLiteral("search"));
    QCOMPARE(service.xp(), qint64(20));
    const int emissions = xpSpy.count();
    service.reportEvent(QStringLiteral("search"));    // 当日重复：幂等丢弃
    QCOMPARE(service.xp(), qint64(20));
    QCOMPARE(xpSpy.count(), emissions);
    QCOMPARE(service.doneTaskCount(), 1);
    // 任务清单镜像 done 位（TasksPage 直接绑该属性）。
    const QVariantList tasks = service.tasks();
    int doneFlags = 0;
    for (const QVariant& row : tasks)
        doneFlags += row.toMap().value("done").toBool() ? 1 : 0;
    QCOMPARE(doneFlags, 1);
}

void ProgressServiceTest::unknownEventsAreIgnored()
{
    ProgressService service(casePhone("0003"));
    service.reportEvent(QStringLiteral("level_up"));
    service.reportEvent(QString());
    QCOMPARE(service.xp(), qint64(0));
}

void ProgressServiceTest::allTasksGrantOnceDailyBonus()
{
    ProgressService service(casePhone("0004"));
    completeAllTasks(service);
    // 30+20+20+20+20+全勤30 = 140。
    QCOMPARE(service.xp(), qint64(140));
    QCOMPARE(service.doneTaskCount(), 5);
    QVERIFY(service.allTasksDone());
    service.reportEvent(QStringLiteral("stats"));     // 全勤后再报：仍幂等
    QCOMPARE(service.xp(), qint64(140));
}

void ProgressServiceTest::levelUpsEmitGiftsPerTier()
{
    ProgressService service(casePhone("0005"));
    service.setTodayForTesting(QStringLiteral("2026-01-01"));
    QSignalSpy levelSpy(&service, &ProgressService::levelUp);
    completeAllTasks(service);                         // 0 → 140：仅跨白银(60)
    QCOMPARE(levelSpy.count(), 1);
    QCOMPARE(service.level(), 2);
    QCOMPARE(levelSpy.at(0).at(0).toInt(), 2);
    QCOMPARE(levelSpy.at(0).at(1).toString(), QString::fromUtf8("白银会员"));
    QCOMPARE(levelSpy.at(0).at(2).toLongLong(), qint64(100));

    // 黄金(150)+铂金(350) 在同一天 140→280 各跨一档；黑金前共 4 次升级。
    service.setTodayForTesting(QStringLiteral("2026-01-02"));
    completeAllTasks(service);                         // 140 → 280
    QCOMPARE(service.level(), 3);
    service.setTodayForTesting(QStringLiteral("2026-01-03"));
    completeAllTasks(service);                         // 280 → 420
    QCOMPARE(service.level(), 4);
    service.setTodayForTesting(QStringLiteral("2026-01-04"));
    completeAllTasks(service);                         // 420 → 560：不跨档
    QCOMPARE(service.level(), 4);
    service.setTodayForTesting(QStringLiteral("2026-01-05"));
    completeAllTasks(service);                         // 560 → 700：黑金门槛
    QCOMPARE(service.level(), 5);
    QCOMPARE(levelSpy.count(), 4);
    QCOMPARE(service.xpToNext(), qint64(0));
    QCOMPARE(service.progress(), 1.0);                 // 黑金恒满条
    QCOMPARE(service.nextTierName(), QString::fromUtf8("已是最高等级"));
    QCOMPARE(service.gifts().size(), 4);               // 礼包逐档入账
    QCOMPARE(service.gifts().at(3).toMap().value("points").toLongLong(), qint64(300));
}

void ProgressServiceTest::crossDayReset()
{
    ProgressService service(casePhone("0006"));
    service.setTodayForTesting(QStringLiteral("2026-01-01"));
    completeAllTasks(service);
    QCOMPARE(service.xp(), qint64(140));
    service.setTodayForTesting(QStringLiteral("2026-01-02"));
    QCOMPARE(service.doneTaskCount(), 0);              // 跨天全部重置
    QVERIFY(!service.allTasksDone());
    completeAllTasks(service);
    QCOMPARE(service.xp(), qint64(280));               // 第二天同样满额
}

void ProgressServiceTest::statePersistsPerPhoneAcrossInstances()
{
    const QString phone = casePhone("0007");
    {
        ProgressService first(phone);
        first.reportEvent(QStringLiteral("checkin"));
        first.reportEvent(QStringLiteral("detail"));
    }
    ProgressService second(phone);                     // 重建实例读回同一分组
    QCOMPARE(second.xp(), qint64(50));
    QCOMPARE(second.doneTaskCount(), 2);
    second.reportEvent(QStringLiteral("detail"));      // 已记事件重启后仍幂等
    QCOMPARE(second.xp(), qint64(50));
}

void ProgressServiceTest::differentPhonesAreIsolated()
{
    ProgressService a(casePhone("0008"));
    a.reportEvent(QStringLiteral("search"));
    ProgressService b(casePhone("0009"));
    QCOMPARE(b.xp(), qint64(0));                       // 换账号不继承成长
}

QTEST_GUILESS_MAIN(ProgressServiceTest)
#include "tst_progress_service.moc"
