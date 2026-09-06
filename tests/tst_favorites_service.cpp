// FavoritesService 单元测试（迭代 3）：收藏切换/查询、QSettings 按用户持久化、
// 切换账号隔离、未登录仅内存态、favoritesChanged 信号口径。
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

    void init()
    {
        service_.setCurrentUser(QStringLiteral("tester-1"));
        service_.resetForTesting();
    }

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

    void invalidIdIsNoOp()
    {
        QSignalSpy spy(&service_, &FavoritesService::favoritesChanged);
        QVERIFY(!service_.toggle(0));
        QVERIFY(!service_.toggle(-7));
        QCOMPARE(spy.count(), 0); // 防御路径不发信号
    }

    void favoriteIdsKeepInsertionOrder()
    {
        service_.toggle(3);
        service_.toggle(1);
        service_.toggle(2);
        QCOMPARE(service_.favoriteIds(), (QVector<qint64>{3, 1, 2}));
        service_.toggle(1); // 取消中间项
        QCOMPARE(service_.favoriteIds(), (QVector<qint64>{3, 2}));
    }

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

    void userSwitchEmitsFavoritesChanged()
    {
        service_.toggle(5);
        QSignalSpy spy(&service_, &FavoritesService::favoritesChanged);
        service_.setCurrentUser(QStringLiteral("tester-2"));
        QCOMPARE(spy.count(), 1); // 集合换主 → 页面需重画星星
        service_.setCurrentUser(QStringLiteral("tester-2"));
        QCOMPARE(spy.count(), 1); // 同值重复注入不发信号
    }

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
