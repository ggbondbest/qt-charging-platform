// ProgressService behaviour (2026-09-09 经验等级批): level curve boundaries,
// day-granular task idempotency, all-tasks bonus, per-tier level-up gifts,
// cross-day reset and QSettings persistence per phone. Pure client-side —
// no transport involved. QSettings domain is isolated to a temp dir so the
// suite never touches the developer's real config.
//
// ---- 中文导读 ----
// boot：QTEST_GUILESS_MAIN 纯逻辑直构（无 GUI/事件循环/网络）。
// 隔离：QSettings 走 Ini 且 User/System 双作用域钉进 QTemporaryDir；
//       每用例一个独立手机号分组（casePhone），互不串档、可任意环境重跑。
// 口径：这是纯客户端等级账（XP/升级礼包只记本机 gifts），与服务端
//       points_ledger（仅 CHECK_IN/RECHARGE）严格分账，本文件全程无 transport。
// 曲线：累计 XP 青铜0/白银60/黄金150/铂金350/黑金700 五档；日任务
//       签到30 + 搜索/详情/路线/月报各20 + 全勤30，满日 140 XP。

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

// 打满当日五任务（五个事件 id 全上报一次）——全勤/跨档用例复用该路径。
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
    // ---- boot：配置隔离域 ----
    // IniFormat 实际落盘走 User scope，System scope 一并钉进 temp dir 作
    // 双保险——保证任何环境下配置都不会写到开发者真机目录。
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

// 测：冷启动全量初始读数；钉五档曲线首档口径（青铜起、xpToNext=60、
// 任务表 5 行 / 阶梯表 5 档、无礼包）。
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

// 测：单事件入账 + 当日重复上报丢弃；钉 xpChanged 只在 XP 真增时发射
// （幂等若仍发信号，页面会白重绘），且 tasks() 的 done 位与计数镜像同步。
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

// 测：陌生事件名（含与信号同名者）与空串静默丢弃；钉 XP 漏斗是白名单制——
// 将来 navigate/信号再多接事件名也不会误加经验。
void ProgressServiceTest::unknownEventsAreIgnored()
{
    ProgressService service(casePhone("0003"));
    service.reportEvent(QStringLiteral("level_up"));
    service.reportEvent(QString());
    QCOMPARE(service.xp(), qint64(0));
}

// 测：五任务打满时全勤 bonus 一次结清（140=30+20×4+30）；钉 bonus 键同样
// 受"当日幂等"管辖——全勤后再补报任何任务事件都不得二次吃 bonus。
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

// 测：跨档结算全谱——单档跨、单日跨双档、不跨档、黑金封顶；钉 levelUp
// 逐档发射且携带 (新等级, 档名, 礼包积分)，礼包 gifts() 逐档入账（白银100、
// 黑金300），封顶后 xpToNext=0 / progress 恒 1.0 / nextTierName 收尾文案。
// 造日用 setTodayForTesting 测试缝（生产不调），避免真等 24 小时。
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

// 测：换"今天"后 done 位/全勤旗标清零而 XP 保留，第二日可再次吃满 bonus；
// 钉跨天重置只清"今日完成表"，不误伤累计成长（done_/bonus 键按日戳判定）。
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

// 测：实例析构→同手机号重建，XP/当日幂等记忆原样读回；钉 save/load 缝
// （构造即 load、变更即 save）——重启丢成长或幂等记忆丢失都会在这里红。
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

// 测：另一手机号实例读到全零；钉分组键 progress/<phone>——换账号必须从零
// 成长（串档 = 上一任机主的等级直接漏给新人）。
void ProgressServiceTest::differentPhonesAreIsolated()
{
    ProgressService a(casePhone("0008"));
    a.reportEvent(QStringLiteral("search"));
    ProgressService b(casePhone("0009"));
    QCOMPARE(b.xp(), qint64(0));                       // 换账号不继承成长
}

QTEST_GUILESS_MAIN(ProgressServiceTest)
#include "tst_progress_service.moc"
