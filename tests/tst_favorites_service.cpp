// FavoritesService 单元测试（迭代 3）：收藏切换/查询、QSettings 按用户持久化、
// 切换账号隔离、未登录仅内存态、favoritesChanged 信号口径。
//
// ---- 中文导读 ----
// boot：QTEST_GUILESS_MAIN 纯逻辑，成员 service_ 直构复用全用例。
// 隔离：配置域用独立 org/app 名与生产分开；每例 init() 换回 tester-1 并
// resetForTesting 清盘、cleanupTestCase 收尾扫尾——用例可任意序执行。
// 口径：收藏是纯本机 QSettings 账（favorites/<userKey>），后端收藏接口尚未
// 定义，本文件不涉及任何 transport。
#include "services/favorites/favorites_service.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QtTest>

using charging::client::services::favorites::FavoritesService;

class FavoritesServiceTest final : public QObject
{
    Q_OBJECT

    FavoritesService service_;

private slots:
    void initTestCase()
    {
        // 独立于生产 app 名的测试配置域，避免污染真实用户收藏（同设置页口径）。
        QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatformTeam"));
        QCoreApplication::setApplicationName(QStringLiteral("FavoritesServiceTest"));
    }

    // 每例复位：换回 tester-1（setCurrentUser 会重读该用户盘上档）再清盘，
    // 上一例的收藏不可能漏进本例。
    void init()
    {
        service_.setCurrentUser(QStringLiteral("tester-1"));
        service_.resetForTesting();
    }

    // ---- 切换/查询基本盘 ----
    // 测：toggle 双向切换 + 返回值 = 操作后的收藏态 + 每次真实变更恰好一发
    // 信号；钉星星控件的显示态与 favoritesChanged 一一对应（漏发=星星不亮）。
    void toggleUpdatesStateAndEmits()
    {
        QSignalSpy spy(&service_, &FavoritesService::favoritesChanged);
        QVERIFY(!service_.contains(101));
        QCOMPARE(service_.favoriteCount(), 0);

        QVERIFY(service_.toggle(101)); // 返回操作后的收藏态
        QVERIFY(service_.contains(101));
        QCOMPARE(service_.favoriteCount(), 1);
        QCOMPARE(spy.count(), 1);

        QVERIFY(!service_.toggle(101)); // 再点取消收藏
        QVERIFY(!service_.contains(101));
        QCOMPARE(service_.favoriteCount(), 0);
        QCOMPARE(spy.count(), 2);
    }

    // 测：id≤0（0 与负数）入参防御；钉非法 id 不入集合、不发信号——
    // 未来后端列表接口给脏数据也不炸星星页。
    void invalidIdIsNoOp()
    {
        QSignalSpy spy(&service_, &FavoritesService::favoritesChanged);
        QVERIFY(!service_.toggle(0));
        QVERIFY(!service_.toggle(-7));
        QCOMPARE(spy.count(), 0); // 防御路径不发信号
    }

    // 测：favoriteIds() 收藏时间正序、移除中间项其余保序；钉收藏夹页列表
    // 口径（若内部改 QSet/排序集合，先收藏的站会跳位——这里红）。
    void favoriteIdsKeepInsertionOrder()
    {
        service_.toggle(3);
        service_.toggle(1);
        service_.toggle(2);
        QCOMPARE(service_.favoriteIds(), (QVector<qint64>{3, 1, 2}));
        service_.toggle(1); // 取消中间项
        QCOMPARE(service_.favoriteIds(), (QVector<qint64>{3, 2}));
    }

    // ---- 持久化与账号隔离 ----
    // 测：同用户新实例从 QSettings 回显全部收藏（≈应用重启）；钉
    // toggle 即落盘、换主即读盘成对——只写不读或只写内存都在这里红。
    void persistenceSurvivesServiceInstance()
    {
        service_.toggle(7);
        service_.toggle(8);
        // 同用户的新实例（≈ 应用重启）从 QSettings 回显。
        FavoritesService reopened;
        reopened.setCurrentUser(QStringLiteral("tester-1"));
        QVERIFY(reopened.contains(7));
        QVERIFY(reopened.contains(8));
        QCOMPARE(reopened.favoriteCount(), 2);
    }

    // 测：切号双向——切走看不到、切回原档还在；钉 favorites/<userKey>
    // 分键换主重读（串档 = A 的收藏亮在 B 的星星上）。
    void userIsolation()
    {
        service_.toggle(42);
        service_.setCurrentUser(QStringLiteral("tester-2")); // 换账号
        QVERIFY(!service_.contains(42));                     // 各见各的收藏
        QCOMPARE(service_.favoriteCount(), 0);
        service_.toggle(99);
        service_.setCurrentUser(QStringLiteral("tester-1")); // 切回
        QVERIFY(service_.contains(42));
        QVERIFY(!service_.contains(99));
    }

    // 测：换用户恰好一发 favoritesChanged、同值重复注入不再发；钉
    // 星星页换主必重画 + 宿主重复 setCurrentUser（如登录态回放）不闪。
    void userSwitchEmitsFavoritesChanged()
    {
        service_.toggle(5);
        QSignalSpy spy(&service_, &FavoritesService::favoritesChanged);
        service_.setCurrentUser(QStringLiteral("tester-2"));
        QCOMPARE(spy.count(), 1); // 集合换主 → 页面需重画星星
        service_.setCurrentUser(QStringLiteral("tester-2"));
        QCOMPARE(spy.count(), 1); // 同值重复注入不发信号
    }

    // 测：空 userKey（未登录）允许内存收藏、新实例读回为空；钉未登录
    // 不落盘不读盘——游客演示不留档，也不捞到上一任登录用户的收藏。
    void guestIsMemoryOnly()
    {
        service_.setCurrentUser(QString()); // 未登录
        QCOMPARE(service_.favoriteCount(), 0);
        QVERIFY(service_.toggle(66));       // 允许内存收藏（本地演示）
        QVERIFY(service_.contains(66));
        FavoritesService reopened;
        reopened.setCurrentUser(QString());
        QVERIFY(!reopened.contains(66));    // 但未落盘
    }

    // 收尾扫盘：本套件真实写过 tester-1/tester-2 两档持久化键（persistence
    // 用例即依赖落盘），全部清掉让每次重跑起点一致。
    void cleanupTestCase()
    {
        FavoritesService cleanup;
        cleanup.setCurrentUser(QStringLiteral("tester-1"));
        cleanup.resetForTesting();
        cleanup.setCurrentUser(QStringLiteral("tester-2"));
        cleanup.resetForTesting();
    }
};

QTEST_GUILESS_MAIN(FavoritesServiceTest)

#include "tst_favorites_service.moc"
