// test_qml_client_pages — QML 页面交互回归（PR #33 二轮评审 5 项 P2 的钉死用例）。
//
// 两层策略：
//   * 脚本控制的 Fake 桥（与真桥同名 signal/invokable）确定性驱动竞态窗口——
//     真 mock 通道 450ms 延迟 + 在途静默丢弃无回执，无法观测"过期响应被丢弃"、
//     "翻页在途下拉后重试仍请求同页"这类只在落定前存在的状态。
//   * 真服务链（QmlApp 自带 MockRequestTransport）走端到端写路径：双改顺序保存、
//     头像键白名单准入、支付 → 顶栏余额同步。
//
// offscreen 下 QQmlComponent 按 file:// 加载页面（与 charging-qml-preview 同机制）。
#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>

#include "charging/client/profile_charging/avatar_library.h"
#include "services/station/station_query_service.h"

#include "app_bridge.h"
#include "service_bridges.h"

using charging::qml::ChargingBridge;
using charging::qml::CouponBridge;
using charging::qml::NotificationBridge;
using charging::qml::OrderBridge;
using charging::qml::PointBridge;
using charging::qml::QmlApp;
using charging::qml::RatingBridge;
using charging::qml::StatsBridge;
using charging::qml::WalletBridge;

namespace {

QVariantMap fakeOrder(qint64 id, const QString& status)
{
    return QVariantMap{
        {QStringLiteral("id"), id},
        {QStringLiteral("orderNo"), QStringLiteral("MOCKORDT%1").arg(id, 4, 10, QChar('0'))},
        {QStringLiteral("status"), status},
        {QStringLiteral("stationName"), QStringLiteral("测试电站")},
        {QStringLiteral("chargerCode"), QStringLiteral("TC-%1").arg(id)},
        {QStringLiteral("energyWh"), 1000},
        {QStringLiteral("durationSeconds"), 600},
        {QStringLiteral("amountCents"), 132},
        {QStringLiteral("unitPriceCentsPerKwh"), 132},
        {QStringLiteral("createdAt"), QStringLiteral("2026-09-01 10:00")},
    };
}

// —— 脚本桥：镜像 OrderBridge 的 QML 可见面（同名方法/信号） ——
class FakeOrderBridge final : public QObject
{
    Q_OBJECT
public:
    struct Call { QString filter; int page; };
    QList<Call> calls;
    int statusCountsCalls = 0;

    Q_INVOKABLE void fetchOrders(const QString& filter, int page)
    {
        if (busy_)
            return; // 真服务同款：在途重复提交静默丢弃
        busy_ = true;
        calls.append({filter, page});
    }
    Q_INVOKABLE void fetchStatusCounts() { ++statusCountsCalls; }
    Q_INVOKABLE bool isFetchingOrders() const { return busy_; }

    void respond(const QVariantList& orders, int total, bool hasMore)
    {
        busy_ = false;
        emit ordersLoaded(orders, total, hasMore);
    }
    void respondFailure()
    {
        busy_ = false;
        emit operationFailed(QStringLiteral("GET_ORDERS"), QStringLiteral("NETWORK"),
                             QStringLiteral("模拟网络故障"));
    }

signals:
    void ordersLoaded(const QVariantList& orders, int total, bool hasMore);
    void statusCountsUpdated(int chargingCount, int waitingPaymentCount, int completedCount);
    void operationFailed(const QString& type, const QString& code, const QString& message);

private:
    bool busy_ = false;
};

// —— 脚本桥：镜像 WalletBridge 的 QML 可见面 ——
class FakeWalletBridge final : public QObject
{
    Q_OBJECT
public:
    QStringList calls; // "nick:<v>" / "avatar:<k>"

    Q_INVOKABLE void fetchProfile() {}
    Q_INVOKABLE bool isUpdatingProfile() const { return false; }
    Q_INVOKABLE void updateNickname(const QString& nickname)
    {
        calls << (QStringLiteral("nick:") + nickname);
    }
    Q_INVOKABLE void updateAvatar(const QString& avatarKey)
    {
        calls << (QStringLiteral("avatar:") + avatarKey);
    }

    void emitProfileLoaded()
    {
        emit profileLoaded(QVariantMap{});
        emit profileUpdated(calls.last().startsWith("nick:") ? "nickname" : "avatar", {});
    }
    void emitReadProfile() { emit profileLoaded(QVariantMap{}); }
    void emitFailure()
    {
        emit operationFailed(QStringLiteral("UPDATE_USER_INFO"), QStringLiteral("MOCK"),
                             QStringLiteral("模拟资料保存失败"));
    }

signals:
    void profileLoaded(const QVariantMap& user);
    void profileUpdated(const QString& field, const QVariantMap& user);
    void rechargeCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void rechargeRecordsLoaded(const QVariantList& records, bool hasMore);
    void operationFailed(const QString& type, const QString& code, const QString& message);
};

// 假月报桥：与 StatsBridge 同名（CONTRACT §1），页面对它无感知。
class FakeStatsBridge final : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void fetchStats(int months = 6,
                                const QString& period = QStringLiteral("month"))
    {
        ++calls;
        lastMonths = months;
        lastPeriod = period;
    }
    Q_INVOKABLE bool isFetchingStats() const { return false; }

    void emitRows(const QVariantList& rows) { emit statsLoaded(rows); }
    void emitFailure()
    {
        emit operationFailed(QStringLiteral("GET_USER_STATS"), QStringLiteral("MOCK"),
                             QStringLiteral("模拟月报加载失败"));
    }

signals:
    void statsLoaded(const QVariantList& months);
    void operationFailed(const QString& type, const QString& code, const QString& message);

public:
    int calls = 0;
    int lastMonths = 0;
    QString lastPeriod;
};

// 批次C 签到/积分桥替身：镜像 PointBridge 的 QML 可见接口。checkIn 不自动回
// 执（由测试显式 emitCheckIn 驱动），fetchPoints 记账参数供断言。
class FakePointsBridge final : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE bool isBusy() const { return false; }
    Q_INVOKABLE void fetchPoints(int page = 1, int pageSize = 20)
    {
        ++fetchCalls;
        lastPage = page;
        lastPageSize = pageSize;
    }
    Q_INVOKABLE void checkIn() { ++checkInCalls; }

    void emitPoints(qint64 points, const QVariantList& entries, int total)
    {
        emit pointsLoaded(points, entries, total);
    }
    void emitCheckIn(const QString& day, qint64 points, qint64 gained, bool already)
    {
        emit checkInCompleted(day, points, gained, already);
    }
    void emitFailure(const QString& type)
    {
        emit operationFailed(type, QStringLiteral("MOCK"), QStringLiteral("模拟积分失败"));
    }

signals:
    void pointsLoaded(qint64 points, const QVariantList& entries, int total);
    void checkInCompleted(const QString& day, qint64 points, qint64 gained,
                          bool alreadyCheckedIn);
    void operationFailed(const QString& type, const QString& code, const QString& message);

public:
    int fetchCalls = 0;
    int checkInCalls = 0;
    int lastPage = 0;
    int lastPageSize = 0;
};

// 批次E 评价桥替身：镜像 RatingBridge 的 QML 可见面。两请求都记账参数、回执
// 由测试显式驱动（FakePointsBridge 同款约定）。
class FakeRatingsBridge final : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE bool isBusy() const { return false; }
    Q_INVOKABLE void fetchMyRatings(int page = 1, int pageSize = 20)
    {
        ++fetchCalls;
        lastPage = page;
        lastPageSize = pageSize;
    }
    Q_INVOKABLE void submitRating(const QString& orderId, int rating, const QString& comment)
    {
        ++submitCalls;
        lastOrderId = orderId;
        lastRating = rating;
        lastComment = comment;
    }

    void emitRatings(const QVariantList& rows, int total) { emit ratingsLoaded(rows, total); }
    void emitSubmitted(const QVariantMap& row, bool already) { emit ratingSubmitted(row, already); }
    void emitFailure(const QString& type)
    {
        emit operationFailed(type, QStringLiteral("MOCK"), QStringLiteral("模拟评价失败"));
    }

signals:
    void ratingsLoaded(const QVariantList& ratings, int total);
    void ratingSubmitted(const QVariantMap& ratingRow, bool alreadyRated);
    void operationFailed(const QString& type, const QString& code, const QString& message);

public:
    int fetchCalls = 0;
    int submitCalls = 0;
    int lastPage = 0;
    int lastPageSize = 0;
    QString lastOrderId;
    int lastRating = 0;
    QString lastComment;
};

// 批次F 扫码页的站点查询桥替身：镜像 StationQueryBridge 的 QML 可见面（请求
// 记账、回执由测试显式驱动；detail 回执 = stationDetailToMap 同形——站字段
// 平铺 + chargers 列表）。
class FakeStationQueryBridge final : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void search(const QString& keyword = QString())
    {
        ++searchCalls;
        lastKeyword = keyword;
    }
    Q_INVOKABLE void fetchDetailById(qint64 stationId, int distanceMeters = -1)
    {
        ++detailCalls;
        lastDetailStationId = stationId;
        lastDetailDistance = distanceMeters;
    }
    Q_INVOKABLE bool isQueryPending() const { return false; }

    void emitStations(const QVariantList& rows) { emit querySucceeded(rows); }
    void emitQueryFailure(const QString& message) { emit queryFailed(message); }
    void emitDetail(const QVariantMap& detail) { emit detailSucceeded(detail); }
    void emitDetailFailure(const QString& message) { emit detailFailed(message); }

signals:
    void queryStarted();
    void querySucceeded(const QVariantList& stations);
    void queryFailed(const QString& message);
    void detailStarted();
    void detailSucceeded(const QVariantMap& detail);
    void detailFailed(const QString& message);

public:
    int searchCalls = 0;
    int detailCalls = 0;
    QString lastKeyword;
    qint64 lastDetailStationId = 0;
    int lastDetailDistance = 0;
};

// 批次F 车辆桥替身：vehicles() 可控列表（准入门第一读数）。无
// activeReservationCount 法 → 页面 call() 失败返回 -1，名额门放行。
class FakeVehicleBridge final : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE QVariantList vehicles() const { return vehicles_; }
    void setVehicleCount(int n)
    {
        vehicles_.clear();
        for (int i = 0; i < n; ++i) {
            vehicles_.push_back(QVariantMap{
                {QStringLiteral("id"), i + 1},
                {QStringLiteral("plate"), QStringLiteral("粤B·1000%1").arg(i + 1)}});
        }
    }

private:
    QVariantList vehicles_;
};

} // namespace

class QmlClientPagesTest final : public QObject
{
    Q_OBJECT

    QQuickWindow* window_ = nullptr;

private slots:
    void init()
    {
        qputenv("CHARGING_CHANNEL", "mock"); // Preview tests opt in; production defaults to TCP.
        if (window_ == nullptr) {
            window_ = new QQuickWindow;
            window_->resize(420, 860);
            window_->show(); // offscreen 下即可：页面与真机同处窗口宿主
        }
    }
    void cleanup()
    {
        delete window_;
        window_ = nullptr;
    }

    QQuickItem* createPage(QQmlEngine& engine, const QString& fileName, QObject* holder)
    {
        QQmlComponent component(&engine);
        component.loadUrl(QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
                                              + QStringLiteral("/pages/profile_charging/")
                                              + fileName));
        if (component.isError())
            qWarning().noquote() << component.errorString();
        if (!component.isReady())
            return nullptr;
        // 与真机同构：页面挂进 QQuickWindow 的 contentItem（Shell 里页面就在
        // 窗口下）。注意：offscreen 下 Repeater 的 delegate 何时挂进 QObject 树
        // 取决于 delegate 组件的异步编译时序（实测：画面已渲染、树里仍查无——
        // grab 走的是场景图快照），因此断言一律不依赖 delegate，只操作页面根
        // 的公开状态（= delegate onClicked 所改写的同一批属性/函数）。
        // holder 声明在最后——槽函数局部变量逆序析构：holder 先亡（带走页面），
        // 再到 window_/engine/app，页面永远死在宿主之前。
        auto* item = qobject_cast<QQuickItem*>(component.create());
        if (item) {
            item->setParent(holder);
            item->setParentItem(window_->contentItem());
            item->setWidth(window_->width());
            item->setHeight(window_->height());
        }
        return item;
    }

    static void click(QObject* target)
    {
        if (target == nullptr) {
            qWarning() << "click target missing";
            return;
        }
        QMetaObject::invokeMethod(target, "clicked"); // 信号发射 = 用户点击语义
    }

    // —— delegate 无关的交互入口 ——
    // 各 delegate 的 onClicked 本质是「改页面根的公开状态 + 调页面函数」；
    // 直接执行同一动作即等价于点击（offscreen 下 delegate 不进 QObject 树）。

    // OrderListPage 筛选胶囊 onClicked: { page.filter = id; load(true) }
    static void selectFilter(QQuickItem* page, const QString& id)
    {
        page->setProperty("filter", id);
        QMetaObject::invokeMethod(page, "load", Q_ARG(QVariant, true));
    }

    // ProfileEditPage 头像格 onClicked: page.avatarKey = key
    static void selectAvatar(QQuickItem* page, const QString& key)
    {
        page->setProperty("avatarKey", key);
    }

    static int cardCount(QQuickItem* page)
    {
        auto* model = page->findChild<QObject*>("uiOrdersModel");
        return model ? model->property("count").toInt() : -1;
    }

private slots:
    void shellBottomBarFollowsLoginState()
    {
        // Exercise the real shell and authentication state; mock is explicit
        // because this regression concerns layout, not TCP authentication.
        QmlApp app(QStringLiteral("127.0.0.1"), 9527, true);
        QQmlEngine engine;
        auto* context = engine.rootContext();
        context->setContextObject(&app); // Service properties retain NOTIFY bindings.
        context->setContextProperty(QStringLiteral("App"), &app);
        context->setContextProperty(QStringLiteral("chargingView"), QStringLiteral("station"));
        context->setContextProperty(QStringLiteral("CHARGING_CHANNEL"), QStringLiteral("mock"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QStringLiteral(CHARGING_QML_SOURCE_DIR) + QStringLiteral("/Shell.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        // The window and all pages are destroyed before their engine/services.
        QScopedPointer<QQuickWindow> shell(qobject_cast<QQuickWindow*>(component.create()));
        QVERIFY(shell);
        auto* tabs = shell->findChild<QQuickItem*>(QStringLiteral("bottomTabBar"));
        auto* stack = shell->findChild<QQuickItem*>(QStringLiteral("pageStack"));
        auto* nav = shell->findChild<QQuickItem*>(QStringLiteral("topNavBar"));
        QVERIFY(tabs); QVERIFY(stack); QVERIFY(nav);
        const auto currentRoute = [stack]() {
            auto* current = stack->property("currentItem").value<QQuickItem*>();
            return current ? current->property("route").toString() : QString();
        };

        QVERIFY(!app.loggedIn());
        QTRY_COMPARE(currentRoute(), QStringLiteral("login"));
        QTRY_VERIFY(!tabs->isVisible());
        QTRY_COMPARE(stack->height(), shell->height() - nav->height());
        QTRY_COMPARE(stack->y() + stack->height(), qreal(shell->height()));

        QSignalSpy rejected(&app, &QmlApp::loginFailed);
        QVERIFY(!app.login(QStringLiteral("123")));
        QCOMPARE(rejected.size(), 1);
        QVERIFY(!app.loggedIn());
        QVERIFY(!tabs->isVisible());
        QCOMPARE(stack->y() + stack->height(), qreal(shell->height()));

        QVERIFY(app.login(QStringLiteral("13800138000")));
        QTRY_COMPARE(currentRoute(), QStringLiteral("station"));
        QTRY_VERIFY(tabs->isVisible());
        QVERIFY(tabs->implicitHeight() > 0);
        QCOMPARE(tabs->height(), tabs->implicitHeight());
        QTRY_COMPARE(stack->height(), shell->height() - nav->height() - tabs->height());
        QTRY_COMPARE(tabs->y() + tabs->height(), qreal(shell->height()));

        // Even an authenticated session must not show navigation on the login route.
        app.navigate(QStringLiteral("login"));
        QTRY_COMPARE(currentRoute(), QStringLiteral("login"));
        QVERIFY(app.loggedIn());
        QTRY_VERIFY(!tabs->isVisible());
        QTRY_COMPARE(stack->y() + stack->height(), qreal(shell->height()));
        app.navigate(QStringLiteral("station"));
        QTRY_VERIFY(tabs->isVisible());

        app.logout();
        QTRY_COMPARE(currentRoute(), QStringLiteral("login"));
        QVERIFY(!app.loggedIn());
        QTRY_VERIFY(!tabs->isVisible());
        QTRY_COMPARE(stack->height(), shell->height() - nav->height());
        QTRY_COMPARE(stack->y() + stack->height(), qreal(shell->height()));
    }

    // P2·复审①：请求中切换筛选——旧("全部")响应不得落到"已完成"列表，
    // 且必须按当前筛选补查（此前按钮改了 filter、请求被吞，旧结果显示）。
    void orderListIgnoresStaleFilterResponse()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeOrderBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("orderService"), &fake);

        QObject holder; // 最后声明 → 最先析构：页面死在 app/engine 之前
        auto* page = createPage(engine, QStringLiteral("OrderListPage.qml"), &holder);
        QVERIFY(page);

        QCOMPARE(fake.calls.size(), 1);
        QCOMPARE(fake.calls.at(0).filter, QStringLiteral("all"));
        QCOMPARE(fake.calls.at(0).page, 1);

        // "全部"在途 → 点「已完成」：只登记意图，不再盲发第二条（会被服务层吞）。
        selectFilter(page, QStringLiteral("completed"));
        QCOMPARE(page->property("filter").toString(), QStringLiteral("completed"));
        QCOMPARE(fake.calls.size(), 1);

        // 过期响应（混着充电中订单的"全部"数据）：不得应用，须立刻重查当前筛选。
        fake.respond({fakeOrder(1, QStringLiteral("charging")),
                      fakeOrder(2, QStringLiteral("completed"))},
                     20, false);

        QCOMPARE(cardCount(page), 0);
        QCOMPARE(fake.calls.size(), 2);
        QCOMPARE(fake.calls.at(1).filter, QStringLiteral("completed"));
        QCOMPARE(fake.calls.at(1).page, 1);

        // 新筛选的响应才允许上屏。
        fake.respond({fakeOrder(2, QStringLiteral("completed"))}, 1, false);
        QCOMPARE(cardCount(page), 1);

        QVERIFY(!page->property("reqActive").toBool());
    }

    // P2·复审②：第 2 页在途时下拉——不得清掉翻页在途态；随后失败，重试仍请求第 2 页。
    void orderListKeepsPagingRetryPageAfterPullFailure()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeOrderBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("orderService"), &fake);

        QObject holder; // 最后声明 → 最先析构：页面死在 app/engine 之前
        auto* page = createPage(engine, QStringLiteral("OrderListPage.qml"), &holder);
        QVERIFY(page);
        fake.respond({fakeOrder(1, QStringLiteral("charging")),
                      fakeOrder(2, QStringLiteral("completed"))},
                     20, true); // 第一页 + hasMore
        QCOMPARE(cardCount(page), 2);
        QCOMPARE(page->property("loadedPage").toInt(), 1);

        auto* more = page->findChild<QQuickItem*>("uiOrderLoadMore");
        QVERIFY(more);
        click(more);
        QCOMPARE(fake.calls.size(), 2);
        QCOMPARE(fake.calls.at(1).page, 2);
        QVERIFY(page->property("loadingMore").toBool());

        // 第 2 页在途中下拉：不发第三条（会被吞），也绝不碰在途翻页的状态。
        auto* pull = page->findChild<QQuickItem*>("uiOrderListStack");
        QVERIFY(pull);
        QMetaObject::invokeMethod(pull, "refreshRequested");
        QCOMPARE(fake.calls.size(), 2);
        QVERIFY(page->property("loadingMore").toBool()); // 原 bug：这里被清成 false，回退失效

        fake.respondFailure(); // 第 2 页失败 → 错误态，但 loadedPage 仍是成功页 1
        QVERIFY(page->property("loadMoreError").toBool());
        QCOMPARE(page->property("loadedPage").toInt(), 1);
        QVERIFY(!page->property("reqActive").toBool());
        QVERIFY(!pull->property("refreshing").toBool());

        // 重试：仍请求第 2 页（原 bug 会跳到第 3 页，漏掉第 2 页）。
        click(more);
        QCOMPARE(fake.calls.size(), 3);
        QCOMPARE(fake.calls.at(2).page, 2);
        fake.respond({fakeOrder(3, QStringLiteral("completed"))}, 3, false);
        QCOMPARE(cardCount(page), 3);
        QCOMPARE(page->property("loadedPage").toInt(), 2);
        QVERIFY(!page->property("loadMoreError").toBool());
    }

    // P2·复审④：同时改昵称+头像 → 串行两步（服务层单飞），全部落定才退出。
    void profileEditSavesBothFieldsSequentially()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeWalletBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("walletService"), &fake);

        QObject holder; // 最后声明 → 最先析构：页面死在 app/engine 之前
        auto* page = createPage(engine, QStringLiteral("ProfileEditPage.qml"), &holder);
        QVERIFY(page);
        QSignalSpy backSpy(&app, &QmlApp::backRequested);

        page->setProperty("nickname", QStringLiteral("小荷"));
        selectAvatar(page, QStringLiteral("cat"));
        QCOMPARE(page->property("avatarKey").toString(), QStringLiteral("cat"));

        click(page->findChild<QQuickItem*>("profileSaveButton"));
        QCOMPARE(fake.calls, QStringList{QStringLiteral("nick:小荷")}); // 头像只登记不抢发
        QCOMPARE(backSpy.count(), 0);

        // An earlier GET_USER_INFO completes while nickname saving is in flight.
        // It must not send avatar prematurely or mark the write as successful.
        fake.emitReadProfile();
        QCOMPARE(fake.calls.size(), 1);
        QCOMPARE(backSpy.count(), 0);
        QVERIFY(page->property("sending").toBool());

        fake.emitProfileLoaded(); // 昵称落定 → 自动补发头像
        QCOMPARE(fake.calls.size(), 2);
        QCOMPARE(fake.calls.at(1), QStringLiteral("avatar:cat"));
        QCOMPARE(backSpy.count(), 0); // 原 bug：第一步成功就退出，头像丢失

        fake.emitReadProfile(); // Reads during the second write are not ACKs either.
        QCOMPARE(backSpy.count(), 0);

        fake.emitProfileLoaded(); // 头像也落定 → 才许退出
        QCOMPARE(backSpy.count(), 1);
        QVERIFY(!page->property("sending").toBool());
    }

    // P2·复审④伴生：任一步失败不得退出，页面留着重试。
    void profileEditStaysWhenSecondStepFails()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeWalletBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("walletService"), &fake);

        QObject holder; // 最后声明 → 最先析构：页面死在 app/engine 之前
        auto* page = createPage(engine, QStringLiteral("ProfileEditPage.qml"), &holder);
        QVERIFY(page);
        QSignalSpy backSpy(&app, &QmlApp::backRequested);
        QSignalSpy toastSpy(&app, &QmlApp::toastRequested);

        page->setProperty("nickname", QStringLiteral("小荷"));
        selectAvatar(page, QStringLiteral("cat"));
        click(page->findChild<QQuickItem*>("profileSaveButton"));
        fake.emitProfileLoaded();
        QCOMPARE(fake.calls.size(), 2);
        fake.emitFailure();

        QCOMPARE(backSpy.count(), 0);
        QVERIFY(!page->property("sending").toBool());          // 保存按钮恢复可点
        QCOMPARE(toastSpy.last().at(1).toString(), QStringLiteral("danger"));
    }

    void chargingStartFailureClearsPendingAndExplainsWhy()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("App", &app);
        engine.rootContext()->setContextProperty("walletService", app.walletService());
        engine.rootContext()->setContextProperty("orderService", app.orderService());
        engine.rootContext()->setContextProperty("chargingService", app.chargingService());
        engine.rootContext()->setContextProperty("reservationService", app.reservationService());
        QObject holder;
        auto* page = createPage(engine, "ChargingHomePage.qml", &holder);
        QVERIFY(page);
        auto* charging = qobject_cast<ChargingBridge*>(app.chargingService());
        QSignalSpy failures(charging, &ChargingBridge::operationFailed);
        QSignalSpy toasts(&app, &QmlApp::toastRequested);
        QVERIFY(QMetaObject::invokeMethod(page, "startReservation", Q_ARG(QVariant, "99999999")));
        QVERIFY(page->property("startPending").toBool());
        QVERIFY(QMetaObject::invokeMethod(page, "startReservation", Q_ARG(QVariant, "99999999")));
        QTRY_VERIFY(!page->property("startPending").toBool());
        QVERIFY(!page->property("loadError").toString().isEmpty());
        QVERIFY(!toasts.isEmpty());
        int startFailures = 0;
        for (const auto& failure : failures)
            if (failure.at(0).toString() == "START_CHARGING") ++startFailures;
        QCOMPARE(startFailures, 1); // The duplicate click did not submit another request.
    }

    // P2·复审③：QML 头像清单与 widgets AvatarLibrary 逐键对拍（双源治理）。
    void avatarChoicesStayInSyncWithAvatarLibrary()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("walletService"),
                                                 app.walletService());

        QObject holder; // 最后声明 → 最先析构：页面死在 app/engine 之前
        auto* page = createPage(engine, QStringLiteral("ProfileEditPage.qml"), &holder);
        QVERIFY(page);
        const QVariantList choices = page->property("avatarChoices").toList();
        QStringList keys;
        QStringList glyphs;
        for (const QVariant& choice : choices) {
            const QVariantMap map = choice.toMap();
            QVERIFY(charging::client::AvatarLibrary::contains(
                map.value(QStringLiteral("key")).toString()));
            keys << map.value(QStringLiteral("key")).toString();
            glyphs << map.value(QStringLiteral("glyph")).toString();
            QVERIFY(!map.value(QStringLiteral("color")).toString().isEmpty());
        }
        QStringList expected;
        for (const charging::client::AvatarSpec& spec : charging::client::AvatarLibrary::all())
            expected << spec.key;
        QCOMPARE(keys, expected);
        QCOMPARE(glyphs.size(), expected.size());
    }

    // 真服务端到端：双改全链（UPDATE_USER_INFO×2 串行）→ 头像键过白名单、
    // currentUser 一致、重开编辑页选中态=持久化键（评审③④要求的"重新加载后一致性"）。
    void profileEditRealMockSavesBothFields()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("walletService"),
                                                 app.walletService());
        auto* wallet = qobject_cast<WalletBridge*>(app.walletService());
        QVERIFY(wallet);
        QSignalSpy failSpy(wallet, &WalletBridge::operationFailed);
        QSignalSpy backSpy(&app, &QmlApp::backRequested);

        QObject holder; // 最后声明 → 最先析构：页面死在 app/engine 之前
        auto* page = createPage(engine, QStringLiteral("ProfileEditPage.qml"), &holder);
        QVERIFY(page);
        QSignalSpy loadedSpy(wallet, &WalletBridge::profileLoaded);
        page->setProperty("nickname", QStringLiteral("小荷"));
        selectAvatar(page, QStringLiteral("cat"));
        click(page->findChild<QQuickItem*>("profileSaveButton"));

        QTRY_VERIFY_WITH_TIMEOUT(loadedSpy.count() >= 2, 6000); // 两次 450ms mock 往返
        if (!failSpy.isEmpty())
            qWarning().noquote() << "bridge operationFailed:" << failSpy.at(0);
        QCOMPARE(backSpy.count(), 1); // 两步全落定才退出
        QCOMPARE(failSpy.count(), 0); // 头像键不在契约白名单会在此报 INVALID_ARGUMENT
        QCOMPARE(app.currentUser().value(QStringLiteral("nickname")).toString(),
                 QStringLiteral("小荷"));
        QCOMPARE(app.currentUser().value(QStringLiteral("avatarKey")).toString(),
                 QStringLiteral("cat"));
        QVERIFY(charging::client::AvatarLibrary::contains(
            app.currentUser().value(QStringLiteral("avatarKey")).toString()));

        // 重开编辑页：选中态来自持久化的 key，而不是展示字形。
        QQmlEngine engine2;
        engine2.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine2.rootContext()->setContextProperty(QStringLiteral("walletService"),
                                                  app.walletService());
        QObject holder2;
        auto* page2 = createPage(engine2, QStringLiteral("ProfileEditPage.qml"), &holder2);
        QVERIFY(page2);
        QCOMPARE(page2->property("avatarKey").toString(), QStringLiteral("cat"));
    }

    // P2·复审⑤：支付成功 → QmlApp 用 paymentCompleted 的余额回写并广播 userChanged。
    // 月报页 × 假桥：进页即拉 6 个月；响应落 ListModel、hero 总计重算、
    // 空态在数据落定后现身；失败回执清在途。
    void statsPageRendersBridgeMonths()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeStatsBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("statsService"), &fake);

        QObject holder; // 最后声明 → 最先析构：页面死在 app/engine 之前
        auto* page = createPage(engine, QStringLiteral("StatsPage.qml"), &holder);
        QVERIFY(page);

        QCOMPARE(fake.calls, 1);
        QCOMPARE(fake.lastMonths, 6);
        QVERIFY(!page->property("loadedOnce").toBool());

        fake.emitRows({QVariantMap{{QStringLiteral("monthKey"), QStringLiteral("2026-09")},
                                   {QStringLiteral("orderCount"), 2},
                                   {QStringLiteral("energyWh"), 20000},
                                   {QStringLiteral("amountCents"), 3000},
                                   {QStringLiteral("durationSeconds"), 3600},
                                   {QStringLiteral("co2Grams"), 11136}}});

        auto* model = page->findChild<QObject*>("uiStatsModel");
        QVERIFY(model);
        QCOMPARE(model->property("count").toInt(), 1);
        QVERIFY(page->property("loadedOnce").toBool());
        QVERIFY(!page->property("reqActive").toBool());

        const QVariantMap totals = page->property("totals").toMap();
        QCOMPARE(totals.value(QStringLiteral("wh")).toInt(), 20000);
        QCOMPARE(totals.value(QStringLiteral("cents")).toInt(), 3000);
        QCOMPARE(totals.value(QStringLiteral("count")).toInt(), 2);
        QCOMPARE(totals.value(QStringLiteral("hours")).toDouble(), 1.0);
        QCOMPARE(totals.value(QStringLiteral("co2Kg")).toDouble(), 11.136);

        // 下拉：刷新请求再发一路；空响应 → 空态可见。
        auto* pull = page->findChild<QQuickItem*>("uiStatsListStack");
        QVERIFY(pull);
        QMetaObject::invokeMethod(pull, "refreshRequested");
        QCOMPARE(fake.calls, 2);
        fake.emitRows({});
        auto* notice = page->findChild<QQuickItem*>("uiStatsEmptyNotice");
        QVERIFY(notice);
        QVERIFY(notice->isVisible());

        // 失败回执：在途清空、页面保留上一次数据（不清屏）。
        QMetaObject::invokeMethod(pull, "refreshRequested");
        QCOMPARE(fake.calls, 3);
        fake.emitFailure();
        QVERIFY(!page->property("reqActive").toBool());
        QVERIFY(page->property("loadedOnce").toBool()); // 失败不回滚已落定状态
        QVERIFY(notice->isVisible());                   // 保留上一次数据（仍空态）

        // 档位切换（批次B）：周报=近 8 周；period 透传给桥；标题联动。
        // delegate 在 offscreen 不可 findChild（CONTRACT.md 已知问题），
        // 走页面级 switchPeriod——与 chip onClicked 同一代码路径。
        QMetaObject::invokeMethod(page, "switchPeriod",
                                  Q_ARG(QVariant, QStringLiteral("week")));
        QCOMPARE(fake.calls, 4);
        QCOMPARE(fake.lastMonths, 8);
        QCOMPARE(fake.lastPeriod, QStringLiteral("week"));
        auto* title = page->findChild<QQuickItem*>("uiStatsTitle");
        QVERIFY(title);
        QCOMPARE(title->property("text").toString(), QStringLiteral("充电周报"));
    }

    // 审查 P2#5 回归：响应在途时切档——陈旧档位响应必须被丢弃（不入模型、
    // caption 不动），并在在途收口后立即补发最后选中的档位；失败回执同款。
    void statsPageIgnoresStalePeriodResponse()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeStatsBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("statsService"), &fake);
        QObject holder;
        auto* page = createPage(engine, QStringLiteral("StatsPage.qml"), &holder);
        QVERIFY(page);
        auto* model = page->findChild<QObject*>("uiStatsModel");
        QVERIFY(model);

        QCOMPARE(fake.calls, 1);                       // 进页自动发月档（在途）
        QMetaObject::invokeMethod(page, "switchPeriod",
                                  Q_ARG(QVariant, QStringLiteral("week")));
        QCOMPARE(fake.calls, 1);                       // 在途不发第二条
        QVERIFY(page->property("reqActive").toBool());

        QVariantMap monthRow;
        monthRow.insert(QStringLiteral("monthKey"), QStringLiteral("2026-09"));
        monthRow.insert(QStringLiteral("orderCount"), 5);
        fake.emitRows({monthRow});                     // 月档陈旧响应到达
        QCOMPARE(model->property("count").toInt(), 0); // 丢弃：不入模型
        QVERIFY(!page->property("loadedOnce").toBool());
        QCOMPARE(page->property("periodCaption").toString(),
                 QStringLiteral("近 6 个月 · 已完成订单"));  // 未接受不动 caption
        QCOMPARE(fake.calls, 2);                       // 立即补发周档
        QCOMPARE(fake.lastPeriod, QStringLiteral("week"));
        QCOMPARE(fake.lastMonths, 8);

        QVariantMap weekRow;
        weekRow.insert(QStringLiteral("monthKey"), QStringLiteral("2026-W36"));
        weekRow.insert(QStringLiteral("orderCount"), 2);
        fake.emitRows({weekRow});
        QCOMPARE(model->property("count").toInt(), 1);
        QVERIFY(page->property("loadedOnce").toBool());
        QCOMPARE(page->property("periodCaption").toString(),
                 QStringLiteral("近 8 周 · 已完成订单"));

        // 失败归属同款：年档在途时切回月档，年档失败不得弹错、不得清屏，
        // 只静默补发当前（月）档位，已落定数据保留。
        QMetaObject::invokeMethod(page, "switchPeriod",
                                  Q_ARG(QVariant, QStringLiteral("year")));
        QCOMPARE(fake.calls, 3);                       // 周档已落定：年档即发
        QMetaObject::invokeMethod(page, "switchPeriod",
                                  Q_ARG(QVariant, QStringLiteral("month")));
        QCOMPARE(fake.calls, 3);                       // 年档在途：不发第二条
        fake.emitFailure();                            // 年档失败回执
        QCOMPARE(fake.calls, 4);                       // 静默补发月档
        QCOMPARE(fake.lastPeriod, QStringLiteral("month"));
        QCOMPARE(model->property("count").toInt(), 1); // 已落定数据未被清屏
        QVERIFY(page->property("loadedOnce").toBool());
        fake.emitRows({monthRow});                     // 月档响应落定
        QCOMPARE(model->property("count").toInt(), 1);
        QVERIFY(page->property("loadedOnce").toBool());
    }

    // 月报桥 × 真 mock 通道：端到端有当月聚合，碳排公式对拍；越界月份 INVALID。
    void statsBridgeEndToEndOnMockChannel()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        auto* stats = qobject_cast<StatsBridge*>(app.statsService());
        QVERIFY(stats);

        QSignalSpy spy(stats, &StatsBridge::statsLoaded);
        stats->fetchStats(6);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 4000);
        const QVariantList months = spy.at(0).at(0).toList();
        QVERIFY(!months.isEmpty()); // mock 种子含当月已完成订单
        const QVariantMap first = months.first().toMap();
        QCOMPARE(first.value(QStringLiteral("monthKey")).toString().size(), 7);
        const double expectedCo2 =
            qRound64(first.value(QStringLiteral("energyWh")).toLongLong() * 0.5568);
        QCOMPARE(first.value(QStringLiteral("co2Grams")).toDouble(), expectedCo2);

        QSignalSpy failSpy(stats, &StatsBridge::operationFailed);
        stats->fetchStats(13); // 越界：contract 层拒
        QTRY_VERIFY_WITH_TIMEOUT(failSpy.count() >= 1, 4000);
        QCOMPARE(failSpy.at(0).at(0).toString(), QStringLiteral("GET_USER_STATS"));
        QCOMPARE(failSpy.at(0).at(1).toString(), QStringLiteral("INVALID_ARGUMENT"));
    }

    // 券桥 × 真 mock 通道：app 接线即拉满缓存（CouponPage 只同步读缓存），
    // 5 张种子券、3 张可用，页面契约字段形态齐。
    void couponBridgeServesMockWallet()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        auto* coupons = qobject_cast<CouponBridge*>(app.couponService());
        QVERIFY(coupons);

        // mock 回执经事件循环延迟派发：QmlApp ctor 的 fetchCoupons 要等一圈
        // 事件才落缓存（QTRY 兼作“接线时自动拉一次”契约本身的验证）。
        QTRY_VERIFY_WITH_TIMEOUT(coupons->couponCount() == 5, 4000);
        const QVariantList rows = coupons->coupons();
        QCOMPARE(rows.size(), 5);
        int available = 0;
        for (const QVariant& row : rows) {
            const QVariantMap item = row.toMap();
            if (item.value(QStringLiteral("status")).toString() == QLatin1String("available"))
                ++available;
            QVERIFY(!item.value(QStringLiteral("id")).toString().isEmpty());
            QVERIFY(item.value(QStringLiteral("expiresAtUtc")).toDouble() > 0.0);
        }
        QCOMPARE(available, 3);
    }

    // 批次C 签到/积分页 × 桥替身：进页自拉流水、流水卡入模、签到回执驱动
    // 按钮三态（未签 → 签到成功 → 重放不反悔），失败回执解锁在途。
    void pointsPageRendersBridgeLedger()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakePointsBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("pointsService"), &fake);

        QObject holder; // 最后声明 → 最先析构（Stats 用例同款时序约定）
        auto* page = createPage(engine, QStringLiteral("PointsPage.qml"), &holder);
        QVERIFY(page);
        QCOMPARE(fake.fetchCalls, 1);
        QCOMPARE(fake.lastPage, 1);
        QCOMPARE(fake.lastPageSize, 20);
        QVERIFY(!page->property("loadedOnce").toBool());

        fake.emitPoints(60, {QVariantMap{{QStringLiteral("id"), QStringLiteral("2")},
                                         {QStringLiteral("amount"), 10},
                                         {QStringLiteral("reason"), QStringLiteral("每日签到")},
                                         {QStringLiteral("createdAtUtc"),
                                          QStringLiteral("2026-09-08T08:00:00.000Z")}},
                            QVariantMap{{QStringLiteral("id"), QStringLiteral("1")},
                                        {QStringLiteral("amount"), 50},
                                        {QStringLiteral("reason"), QStringLiteral("注册礼包")},
                                        {QStringLiteral("createdAtUtc"),
                                         QStringLiteral("2026-09-05T08:00:00.000Z")}}}, 2);
        auto* model = page->findChild<QObject*>("uiPointsModel");
        QVERIFY(model);
        QCOMPARE(model->property("count").toInt(), 2);
        QCOMPARE(page->property("points").toInt(), 60);
        QVERIFY(page->property("loadedOnce").toBool());
        QVERIFY(!page->property("reqActive").toBool());

        // 签到按钮路径（delegate 在 offscreen 不可 findChild——走页面函数，
        // 与 onClicked 同一代码路径）：请求发出、checkingIn 置真。
        QMetaObject::invokeMethod(page, "checkInNow");
        QCOMPARE(fake.checkInCalls, 1);
        QVERIFY(page->property("checkingIn").toBool());

        // 成功回执：总分更新、按钮进入"今日已签"态（重放同样置真，不反悔）。
        fake.emitCheckIn(QStringLiteral("2026-09-08"), 70, 10, false);
        QVERIFY(!page->property("checkingIn").toBool());
        QCOMPARE(page->property("points").toInt(), 70);
        QVERIFY(page->property("todayCheckedIn").toBool());

        auto* title = page->findChild<QQuickItem*>("uiPointsTitle");
        QVERIFY(title);
        QCOMPARE(title->property("text").toString(), QStringLiteral("签到 · 积分"));

        // 失败回执（CHECK_IN 在途挂掉）：checkingIn 解锁、已签态保持。
        QMetaObject::invokeMethod(page, "checkInNow");   // todayCheckedIn 拦路：不发请求
        QCOMPARE(fake.checkInCalls, 1);
        fake.emitFailure(QStringLiteral("GET_POINTS"));
        QVERIFY(!page->property("reqActive").toBool());
    }

    // 签到桥 × 真 mock 通道端到端：种子礼包 50 → 签到 +10 → 流水两行新在前。
    void pointsBridgeEndToEndOnMockChannel()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        auto* points = qobject_cast<PointBridge*>(app.pointsService());
        QVERIFY(points);

        QSignalSpy loaded(points, &PointBridge::pointsLoaded);
        points->fetchPoints();
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 4000);
        QCOMPARE(loaded.at(0).at(0).toLongLong(), 50);   // 注册礼包 seed
        QCOMPARE(loaded.at(0).at(2).toInt(), 1);

        QSignalSpy done(points, &PointBridge::checkInCompleted);
        points->checkIn();
        QTRY_VERIFY_WITH_TIMEOUT(done.count() >= 1, 4000);
        QCOMPARE(done.at(0).at(1).toLongLong(), 60);
        QCOMPARE(done.at(0).at(2).toLongLong(), 10);
        QCOMPARE(done.at(0).at(3).toBool(), false);
        QCOMPARE(done.at(0).at(0).toString().size(), 10);  // "YYYY-MM-DD"

        // 同日重放：already=true、gained=0（幂等镜像）。
        points->checkIn();
        QTRY_VERIFY_WITH_TIMEOUT(done.count() >= 2, 4000);
        QCOMPARE(done.at(1).at(2).toLongLong(), 0);
        QCOMPARE(done.at(1).at(3).toBool(), true);

        loaded.clear();
        points->fetchPoints(1, 5);
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 4000);
        QCOMPARE(loaded.at(0).at(1).toList().size(), 2);
        QCOMPARE(loaded.at(0).at(1).toList().first().toMap()
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("每日签到"));              // 新→旧
    }

    // 批次E 我的评价页 × 桥替身：进页自拉列表、评价卡入模、失败回执解锁在途。
    void ratingsPageRendersBridgeRows()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeRatingsBridge fake;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("ratingsService"), &fake);

        QObject holder; // 最后声明 → 最先析构（积分页用例同款时序约定）
        auto* page = createPage(engine, QStringLiteral("RatingsPage.qml"), &holder);
        QVERIFY(page);
        QCOMPARE(fake.fetchCalls, 1);
        QCOMPARE(fake.lastPage, 1);
        QCOMPARE(fake.lastPageSize, 20);
        QVERIFY(!page->property("loadedOnce").toBool());

        fake.emitRatings({QVariantMap{{QStringLiteral("id"), QStringLiteral("2")},
                                      {QStringLiteral("orderId"), QStringLiteral("3")},
                                      {QStringLiteral("chargerId"), QStringLiteral("12")},
                                      {QStringLiteral("chargerCode"), QStringLiteral("B07")},
                                      {QStringLiteral("stationName"),
                                       QStringLiteral("云杉科技园区充电站")},
                                      {QStringLiteral("rating"), 5},
                                      {QStringLiteral("comment"),
                                       QStringLiteral("充电很快，环境不错。")},
                                      {QStringLiteral("createdAtUtc"),
                                       QStringLiteral("2026-09-08T08:00:00.000Z")}},
                          QVariantMap{{QStringLiteral("id"), QStringLiteral("1")},
                                      {QStringLiteral("orderId"), QStringLiteral("2")},
                                      {QStringLiteral("chargerId"), QStringLiteral("11")},
                                      {QStringLiteral("chargerCode"), QStringLiteral("A03")},
                                      {QStringLiteral("stationName"),
                                       QStringLiteral("云杉科技园区充电站")},
                                      {QStringLiteral("rating"), 4},
                                      {QStringLiteral("comment"), QString()},
                                      {QStringLiteral("createdAtUtc"),
                                       QStringLiteral("2026-09-07T08:00:00.000Z")}}}, 2);
        auto* model = page->findChild<QObject*>("uiRatingsModel");
        QVERIFY(model);
        QCOMPARE(model->property("count").toInt(), 2);
        QVERIFY(page->property("loadedOnce").toBool());
        QVERIFY(!page->property("reqActive").toBool());

        auto* title = page->findChild<QQuickItem*>("uiRatingsTitle");
        QVERIFY(title);
        QCOMPARE(title->property("text").toString(), QStringLiteral("我的评价"));

        // 失败回执解锁在途；无关类型的失败不碰本页状态。
        fake.emitFailure(QStringLiteral("GET_POINTS"));
        fake.emitFailure(QStringLiteral("GET_MY_RATINGS"));
        QVERIFY(!page->property("reqActive").toBool());
    }

    // 批次E 订单详情页完成态评价卡 × 桥替身：arg 补完成单自拉存量、未命中保
    // 持可编辑形态、星级门槛拦提交、成功回执切"已评价"只读、已评价后不重拉。
    void orderDetailCompletedRatingCardFlow()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeRatingsBridge fake;
        FakeOrderBridge orders;   // ensureData 兜底拉单会打这里（只记账，不应答）
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("orderService"), &orders);
        engine.rootContext()->setContextProperty(QStringLiteral("chargingService"),
                                                 app.chargingService());
        engine.rootContext()->setContextProperty(QStringLiteral("ratingsService"), &fake);
        QObject holder;
        auto* page = createPage(engine, QStringLiteral("OrderDetailPage.qml"), &holder);
        QVERIFY(page);

        const QVariantMap completed{
            {QStringLiteral("id"), QStringLiteral("77")},
            {QStringLiteral("orderNo"), QStringLiteral("TEST-77")},
            {QStringLiteral("status"), QStringLiteral("completed")},
            {QStringLiteral("amountCents"), 2400},
            {QStringLiteral("chargerCode"), QStringLiteral("B07")},
            {QStringLiteral("stationName"), QStringLiteral("云杉科技园区充电站")}};
        page->setProperty("arg", completed);     // onArgChanged → loadRating
        QCOMPARE(fake.fetchCalls, 1);
        QCOMPARE(fake.lastPageSize, 20);

        QVariantMap other;                       // 非命中行（别的订单）
        other.insert(QStringLiteral("orderId"), QStringLiteral("9"));
        fake.emitRatings({other}, 1);
        QVERIFY(page->property("myRating").isNull());
        QVERIFY(!page->property("ratingReqActive").toBool());

        QMetaObject::invokeMethod(page, "submitRatingNow");   // 未选星：门槛拦截
        QCOMPARE(fake.submitCalls, 0);
        page->setProperty("pickedStars", 4);
        QMetaObject::invokeMethod(page, "submitRatingNow");
        QCOMPARE(fake.submitCalls, 1);
        QCOMPARE(fake.lastOrderId, QStringLiteral("77"));
        QCOMPARE(fake.lastRating, 4);
        QVERIFY(page->property("ratingSubmitting").toBool());

        QVariantMap mine;
        mine.insert(QStringLiteral("orderId"), QStringLiteral("77"));
        mine.insert(QStringLiteral("rating"), 4);
        fake.emitSubmitted(mine, false);
        QVERIFY(!page->property("ratingSubmitting").toBool());
        QVERIFY(!page->property("myRating").isNull());

        fake.emitFailure(QStringLiteral("SUBMIT_CHARGER_RATING"));  // 解锁不悔已评态
        QVERIFY(!page->property("myRating").isNull());

        const int fetchBefore = fake.fetchCalls;   // 已评后幂等不重拉
        QMetaObject::invokeMethod(page, "loadRating");
        QCOMPARE(fake.fetchCalls, fetchBefore);
    }

    // 审查 P2#6 回归：提交 A 在途 → 切到订单 B → A 的迟到响应不得写入 B 页
    // （不污染 myRating、不误弹 toast），且不得卡死 B 的提交入口。
    void orderDetailIgnoresRatingResponseForStaleOrder()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeRatingsBridge fake;
        FakeOrderBridge orders;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("orderService"), &orders);
        engine.rootContext()->setContextProperty(QStringLiteral("chargingService"),
                                                 app.chargingService());
        engine.rootContext()->setContextProperty(QStringLiteral("ratingsService"), &fake);
        QObject holder;
        auto* page = createPage(engine, QStringLiteral("OrderDetailPage.qml"), &holder);
        QVERIFY(page);

        QVariantMap orderA;
        orderA.insert(QStringLiteral("id"), QStringLiteral("77"));
        orderA.insert(QStringLiteral("status"), QStringLiteral("completed"));
        orderA.insert(QStringLiteral("amountCents"), 2400);
        page->setProperty("arg", orderA);

        // 提交 A（响应留在在途窗口）。
        page->setProperty("pickedStars", 4);
        QMetaObject::invokeMethod(page, "submitRatingNow");
        QCOMPARE(fake.submitCalls, 1);
        QCOMPARE(fake.lastOrderId, QStringLiteral("77"));
        QVERIFY(page->property("ratingSubmitting").toBool());

        // 用户切到订单 B：页面实例复用，展示态必须清零。
        QVariantMap orderB = orderA;
        orderB.insert(QStringLiteral("id"), QStringLiteral("88"));
        page->setProperty("arg", orderB);
        QVERIFY(page->property("myRating").isNull());
        QCOMPARE(page->property("pickedStars").toInt(), 0);

        // A 的迟到响应：归属不匹配 → 只收口在途标志，不写 B 页。
        QVariantMap rowA;
        rowA.insert(QStringLiteral("orderId"), QStringLiteral("77"));
        rowA.insert(QStringLiteral("rating"), 4);
        fake.emitSubmitted(rowA, false);
        QVERIFY(page->property("myRating").isNull());             // 未污染
        QVERIFY(!page->property("ratingSubmitting").toBool());    // 未卡死

        // B 自身提交正常：成功响应归属匹配 → 正常落页。
        page->setProperty("pickedStars", 5);
        QMetaObject::invokeMethod(page, "submitRatingNow");
        QCOMPARE(fake.submitCalls, 2);
        QCOMPARE(fake.lastOrderId, QStringLiteral("88"));
        QVariantMap rowB;
        rowB.insert(QStringLiteral("orderId"), QStringLiteral("88"));
        rowB.insert(QStringLiteral("rating"), 5);
        fake.emitSubmitted(rowB, false);
        QVERIFY(!page->property("myRating").isNull());
        QCOMPARE(page->property("myRating").toMap()
                     .value(QStringLiteral("rating")).toInt(), 5);

        // 失败回执同款：提交 B（响应在途）→ 切到 C → B 的失败迟到：
        // 只收口在途标志，不误弹到 C 页，更不卡 C 的提交入口。
        QVariantMap orderC = orderA;
        orderC.insert(QStringLiteral("id"), QStringLiteral("99"));
        page->setProperty("pickedStars", 3);
        QMetaObject::invokeMethod(page, "submitRatingNow");
        QCOMPARE(fake.submitCalls, 3);
        QCOMPARE(fake.lastOrderId, QStringLiteral("88"));
        page->setProperty("arg", orderC);
        fake.emitFailure(QStringLiteral("SUBMIT_CHARGER_RATING"));  // B 的失败迟到
        QVERIFY(!page->property("ratingSubmitting").toBool());      // 在途收口
        QVERIFY(page->property("myRating").isNull());               // 失败不写状态
        page->setProperty("pickedStars", 2);
        QMetaObject::invokeMethod(page, "submitRatingNow");         // C 正常提交
        QCOMPARE(fake.submitCalls, 4);
        QCOMPARE(fake.lastOrderId, QStringLiteral("99"));
    }

    // 评价桥 × 真 mock 通道端到端：种子 1 行（id=3 完成单）→ 订单 2 首评 →
    // 重放幂等 → 列表两行新在前（trim 在链路上生效）。
    void ratingsBridgeEndToEndOnMockChannel()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        auto* ratings = qobject_cast<RatingBridge*>(app.ratingsService());
        QVERIFY(ratings);

        QSignalSpy loaded(ratings, &RatingBridge::ratingsLoaded);
        ratings->fetchMyRatings();
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 4000);
        QCOMPARE(loaded.at(0).at(0).toList().size(), 1);
        QCOMPARE(loaded.at(0).at(0).toList().first().toMap()
                     .value(QStringLiteral("orderId")).toString(), QStringLiteral("3"));
        QCOMPARE(loaded.at(0).at(1).toInt(), 1);

        QSignalSpy done(ratings, &RatingBridge::ratingSubmitted);
        ratings->submitRating(QStringLiteral("2"), 4, QStringLiteral("  还行  "));
        QTRY_VERIFY_WITH_TIMEOUT(done.count() >= 1, 4000);
        QCOMPARE(done.at(0).at(1).toBool(), false);
        QCOMPARE(done.at(0).at(0).toMap().value(QStringLiteral("comment")).toString(),
                 QStringLiteral("还行"));
        QCOMPARE(done.at(0).at(0).toMap().value(QStringLiteral("chargerCode")).toString(),
                 QStringLiteral("A03"));

        // 重放：alreadyRated=true、首评原值（幂等镜像）。
        ratings->submitRating(QStringLiteral("2"), 1, QString());
        QTRY_VERIFY_WITH_TIMEOUT(done.count() >= 2, 4000);
        QCOMPARE(done.at(1).at(1).toBool(), true);
        QCOMPARE(done.at(1).at(0).toMap().value(QStringLiteral("rating")).toInt(), 4);

        loaded.clear();
        ratings->fetchMyRatings(1, 5);
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 4000);
        QCOMPARE(loaded.at(0).at(0).toList().size(), 2);
        QCOMPARE(loaded.at(0).at(0).toList().first().toMap()
                     .value(QStringLiteral("orderId")).toString(), QStringLiteral("2"));
    }

    void paymentSyncsTopBarBalance()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        auto* orders = qobject_cast<OrderBridge*>(app.orderService());
        auto* charging = qobject_cast<ChargingBridge*>(app.chargingService());
        QVERIFY(orders && charging);

        QSignalSpy ordersSpy(orders, &OrderBridge::ordersLoaded);
        orders->fetchOrders(QStringLiteral("waiting_payment"), 1);
        QTRY_VERIFY_WITH_TIMEOUT(ordersSpy.count() >= 1, 4000);
        const QVariantList page1 = ordersSpy.at(0).at(0).toList();
        QVERIFY(!page1.isEmpty());
        const qint64 orderId = page1.first().toMap().value(QStringLiteral("id")).toLongLong();
        const qint64 balanceBefore =
            app.currentUser().value(QStringLiteral("balanceCents")).toLongLong();
        QVERIFY(balanceBefore > 0);

        QSignalSpy paySpy(charging, &ChargingBridge::paymentCompleted);
        QSignalSpy userSpy(&app, &QmlApp::userChanged);
        charging->payOrder(orderId);
        QTRY_VERIFY_WITH_TIMEOUT(paySpy.count() >= 1, 4000);

        const qint64 paidAfter = paySpy.at(0).at(1).toLongLong();
        QVERIFY(paidAfter < balanceBefore); // 真实扣款，非幂等重放
        QCOMPARE(app.currentUser().value(QStringLiteral("balanceCents")).toLongLong(),
                 paidAfter);                 // 顶栏绑定源立即一致
        QVERIFY(userSpy.count() >= 1);        // userChanged 已广播（TopNavBar 重绑）
    }

    // 审查 P2#3 回归（券侧）：同会话充值 ≥¥50 → mock 按服务端规则发券，
    // 但桥缓存停在 boot 拉取形态；navigate("coupon") 进页强制补拉后到账。
    void couponWalletRefetchesOnEntryAfterRecharge()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        auto* coupons = qobject_cast<CouponBridge*>(app.couponService());
        auto* wallet = qobject_cast<WalletBridge*>(app.walletService());
        QVERIFY(coupons && wallet);

        // boot 拉取落定 = 种子 5 张（此前用例已钉过形态，这里只取基线）。
        QTRY_COMPARE_WITH_TIMEOUT(coupons->coupons().size(), 5, 4000);

        QSignalSpy rechargeSpy(wallet, &WalletBridge::rechargeCompleted);
        wallet->recharge(6000);                       // ¥60 ≥ ¥50 门槛
        QTRY_VERIFY_WITH_TIMEOUT(rechargeSpy.count() >= 1, 4000);
        QCOMPARE(coupons->coupons().size(), 5);       // 病灶：缓存未感知新券

        app.navigate(QStringLiteral("coupon"));       // 进页 → 强制补拉
        QTRY_COMPARE_WITH_TIMEOUT(coupons->coupons().size(), 6, 4000);
        QCOMPARE(coupons->coupons().first().toMap()
                     .value(QStringLiteral("title")).toString(),
                 QStringLiteral("充值回馈 ¥5 充电券"));   // 新在前（prepend 口径）
    }

    // 审查 P2#3 回归（通知侧）：同会话支付成功 → mock payOrder 落 order_paid
    // 通知；navigate("notifications") 进页强制补拉后新行到账。
    void notificationsRefetchOnEntryAfterPayment()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        auto* notifications = qobject_cast<NotificationBridge*>(app.notificationService());
        auto* orders = qobject_cast<OrderBridge*>(app.orderService());
        auto* charging = qobject_cast<ChargingBridge*>(app.chargingService());
        QVERIFY(notifications && orders && charging);

        QSignalSpy ordersSpy(orders, &OrderBridge::ordersLoaded);
        orders->fetchOrders(QStringLiteral("waiting_payment"), 1);
        QTRY_VERIFY_WITH_TIMEOUT(ordersSpy.count() >= 1, 4000);
        const QVariantList page1 = ordersSpy.at(0).at(0).toList();
        QVERIFY(!page1.isEmpty());
        const qint64 orderId = page1.first().toMap().value(QStringLiteral("id")).toLongLong();

        QSignalSpy paySpy(charging, &ChargingBridge::paymentCompleted);
        charging->payOrder(orderId);
        QTRY_VERIFY_WITH_TIMEOUT(paySpy.count() >= 1, 4000);
        const int before = notifications->notifications().size();  // 仍为 boot 形态

        QSignalSpy notifSpy(notifications, &NotificationBridge::notificationsChanged);
        app.navigate(QStringLiteral("notifications"));             // 进页 → 强制补拉
        QTRY_VERIFY_WITH_TIMEOUT(notifSpy.count() >= 1, 4000);
        QCOMPARE(notifications->notifications().size(), before + 1);
        QCOMPARE(notifications->notifications().first().toMap()
                     .value(QStringLiteral("type")).toString(),
                 QStringLiteral("order_paid"));
    }

    // 批次F 扫码页 × 桥替身全状态机：入场拉站、速选码→detail 取首台空闲桩、
    // 手输 CHG://站/桩 精确匹配、零车辆也交给统一订单准入门、导航坐标完整透传、
    // 非法码本地判 miss 不发请求。
    void scanPageReserveFlowOnBridgeFake()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        app.clearUnfinishedOrdersForTesting(); // Only the explicit mock transport is changed.
        FakeStationQueryBridge query;
        FakeVehicleBridge vehicles;
        QObject reservations;   // No local vehicle/reservation-count bridge is required.
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("stationQueryService"), &query);
        engine.rootContext()->setContextProperty(QStringLiteral("settingsService"), &vehicles);
        engine.rootContext()->setContextProperty(QStringLiteral("reservationService"),
                                                 &reservations);
        QObject holder;
        auto* page = createPage(engine, QStringLiteral("ScanPage.qml"), &holder);
        QVERIFY(page);
        QCOMPARE(query.searchCalls, 1);

        query.emitStations({
            QVariantMap{{QStringLiteral("id"), 5LL},
                        {QStringLiteral("name"), QStringLiteral("后海城市广场站")},
                        {QStringLiteral("priceCentsPerKwh"), 105},
                        {QStringLiteral("distanceMeters"), 3200}},
            QVariantMap{{QStringLiteral("id"), 6LL},
                        {QStringLiteral("name"), QStringLiteral("西丽湖临时站")},
                        {QStringLiteral("priceCentsPerKwh"), 92},
                        {QStringLiteral("distanceMeters"), 3800}}});
        QCOMPARE(page->property("stations").toList().size(), 2);

        // 速选第二站（index 1）→ detail 平铺形 + chargers（首台故障、次台空闲）
        QMetaObject::invokeMethod(page, "scanStation", Q_ARG(QVariant, 1));
        QCOMPARE(query.detailCalls, 1);
        QCOMPARE(query.lastDetailStationId, 6LL);
        QCOMPARE(query.lastDetailDistance, 3800);
        QCOMPARE(page->property("phase").toString(), QStringLiteral("searching"));
        query.emitDetail(QVariantMap{
            {QStringLiteral("id"), 6LL},
            {QStringLiteral("name"), QStringLiteral("西丽湖临时站")},
            {QStringLiteral("priceCentsPerKwh"), 92},
            {QStringLiteral("distanceMeters"), 3800},
            {QStringLiteral("latitude"), 38.88},
            {QStringLiteral("longitude"), 121.53},
            {QStringLiteral("chargers"), QVariantList{
                QVariantMap{{QStringLiteral("id"), 61}, {QStringLiteral("code"), QStringLiteral("L01")},
                            {QStringLiteral("type"), QStringLiteral("slow")},
                            {QStringLiteral("powerWatts"), 7000}, {QStringLiteral("status"), QStringLiteral("fault")}},
                QVariantMap{{QStringLiteral("id"), 62}, {QStringLiteral("code"), QStringLiteral("L02")},
                            {QStringLiteral("type"), QStringLiteral("fast")},
                            {QStringLiteral("powerWatts"), 120000}, {QStringLiteral("status"), QStringLiteral("available")}}}}});
        QCOMPARE(page->property("phase").toString(), QStringLiteral("found"));
        QVERIFY(!page->property("reqActive").toBool());
        QCOMPARE(page->property("foundCharger").toMap().value(QStringLiteral("code")).toString(),
                 QStringLiteral("L02"));   // 跳过故障桩取首台空闲

        // 零车辆不挡预约；先查未完成订单，不能直接跳确认页或要求去设置加车。
        QVERIFY(vehicles.vehicles().isEmpty());
        QSignalSpy routes(&app, &QmlApp::navigateRequested);
        QVERIFY(QMetaObject::invokeMethod(page, "reserveNow"));
        QVERIFY(app.checkingOrders());
        QCOMPARE(routes.size(), 0);
        QVERIFY(QMetaObject::invokeMethod(page, "reserveNow")); // rapid second submit is ignored
        QTRY_COMPARE_WITH_TIMEOUT(routes.size(), 1, 4000);
        QVERIFY(!app.checkingOrders());
        QCOMPARE(routes.first().at(0).toString(), QStringLiteral("reservation_confirm"));
        const QVariantMap arg = page->property("reserveArg").toMap();
        QCOMPARE(arg.value(QStringLiteral("stationId")).toString(), QStringLiteral("6"));
        QCOMPARE(arg.value(QStringLiteral("stationName")).toString(),
                 QStringLiteral("西丽湖临时站"));
        QCOMPARE(arg.value(QStringLiteral("chargerCode")).toString(), QStringLiteral("L02"));
        QCOMPARE(arg.value(QStringLiteral("chargerType")).toString(), QStringLiteral("fast"));
        QCOMPARE(arg.value(QStringLiteral("chargerPowerWatts")).toInt(), 120000);
        QCOMPARE(arg.value(QStringLiteral("priceCentsPerKwh")).toInt(), 92);
        QCOMPARE(arg.value(QStringLiteral("distanceMeters")).toInt(), 3800);
        QCOMPARE(arg.value(QStringLiteral("chargerId")).toString(), QStringLiteral("62"));
        QCOMPARE(arg.value(QStringLiteral("stationId")).metaType().id(), QMetaType::QString);
        QCOMPARE(arg.value(QStringLiteral("chargerId")).metaType().id(), QMetaType::QString);
        QCOMPARE(arg.value(QStringLiteral("stationLatitude")).toDouble(), 38.88);
        QCOMPARE(arg.value(QStringLiteral("stationLongitude")).toDouble(), 121.53);
        QVERIFY(arg.value(QStringLiteral("hasStationLocation")).toBool());
        QCOMPARE(routes.first().at(1).toMap(), arg);

        // 手输：非法码本地 miss 不发请求；CHG://站/桩 精确取桩（不走"首台空闲"）。
        page->setProperty("phase", QStringLiteral("idle"));
        QMetaObject::invokeMethod(page, "scanCode", Q_ARG(QVariant, QStringLiteral("abc")));
        QCOMPARE(page->property("phase").toString(), QStringLiteral("miss"));
        QCOMPARE(query.detailCalls, 1);

        QMetaObject::invokeMethod(page, "scanCode", Q_ARG(QVariant, QStringLiteral("CHG://1/7")));
        QCOMPARE(query.detailCalls, 2);
        QCOMPARE(query.lastDetailStationId, 1LL);
        query.emitDetail(QVariantMap{
            {QStringLiteral("id"), 1LL},
            {QStringLiteral("name"), QStringLiteral("科技园充电驿站")},
            {QStringLiteral("priceCentsPerKwh"), 120},
            {QStringLiteral("chargers"), QVariantList{
                QVariantMap{{QStringLiteral("id"), 7}, {QStringLiteral("code"), QStringLiteral("K07")},
                            {QStringLiteral("type"), QStringLiteral("fast")},
                            {QStringLiteral("powerWatts"), 180000}, {QStringLiteral("status"), QStringLiteral("charging")}}}}});
        QCOMPARE(page->property("phase").toString(), QStringLiteral("found"));
        // 码上桩占用中：可展示但预约门拦下（arg 不更新——保留上一份 6/L02）。
        QMetaObject::invokeMethod(page, "reserveNow");
        QCOMPARE(page->property("reserveArg").toMap().value(QStringLiteral("chargerCode")).toString(),
                 QStringLiteral("L02"));
        QCOMPARE(routes.size(), 1); // occupied charger did not submit another order check
    }

    void scanPageZeroVehiclesCannotBypassUnfinishedOrder()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        // Keep the seeded charging order. The page must delegate rather than
        // deciding that no vehicle/no local reservation means it can reserve.
        FakeStationQueryBridge query;
        FakeVehicleBridge vehicles;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("stationQueryService"), &query);
        engine.rootContext()->setContextProperty(QStringLiteral("settingsService"), &vehicles);
        QObject holder;
        auto* page = createPage(engine, QStringLiteral("ScanPage.qml"), &holder);
        QVERIFY(page);
        query.emitStations({});
        QVERIFY(page->setProperty("phase", QStringLiteral("found")));
        QVERIFY(page->setProperty("foundStation", QVariantMap{
            {QStringLiteral("id"), QStringLiteral("1")},
            {QStringLiteral("name"), QStringLiteral("测试站")},
            {QStringLiteral("priceCentsPerKwh"), 120}}));
        QVERIFY(page->setProperty("foundCharger", QVariantMap{
            {QStringLiteral("id"), QStringLiteral("7")},
            {QStringLiteral("code"), QStringLiteral("TEST-7")},
            {QStringLiteral("type"), QStringLiteral("fast")},
            {QStringLiteral("powerWatts"), 120000},
            {QStringLiteral("status"), QStringLiteral("available")}}));
        QSignalSpy routes(&app, &QmlApp::navigateRequested);
        QVERIFY(vehicles.vehicles().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(page, "reserveNow"));
        QVERIFY(app.checkingOrders());
        const QVariantMap arg = page->property("reserveArg").toMap();
        QVERIFY(!arg.isEmpty());
        QVERIFY(!arg.value(QStringLiteral("hasStationLocation")).toBool());
        QCOMPARE(arg.value(QStringLiteral("distanceMeters")).toInt(), -1);
        QTRY_COMPARE_WITH_TIMEOUT(routes.size(), 1, 4000);
        QCOMPARE(routes.first().at(0).toString(), QStringLiteral("charging"));
        QVERIFY(!app.checkingOrders());
    }

    // 批次F 扫码页失败面：查询失败解锁、detail 失败 miss、站无空闲桩 miss、
    // 码上桩不存在 miss。
    void scanPageMissPaths()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        FakeStationQueryBridge query;
        FakeVehicleBridge vehicles;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("stationQueryService"), &query);
        engine.rootContext()->setContextProperty(QStringLiteral("settingsService"), &vehicles);
        QObject holder;
        auto* page = createPage(engine, QStringLiteral("ScanPage.qml"), &holder);
        QVERIFY(page);

        query.emitQueryFailure(QStringLiteral("站点查询服务暂时不可用"));
        QVERIFY(!page->property("reqActive").toBool());
        QVERIFY(page->property("loadedOnce").toBool());

        QMetaObject::invokeMethod(page, "scanCode", Q_ARG(QVariant, QStringLiteral("99")));
        QCOMPARE(page->property("phase").toString(), QStringLiteral("searching"));
        query.emitDetailFailure(QStringLiteral("未找到该站点信息"));
        QCOMPARE(page->property("phase").toString(), QStringLiteral("miss"));
        QCOMPARE(page->property("failMessage").toString(),
                 QStringLiteral("未找到该站点信息"));

        // 全占用站 + 未指定桩 → "暂无空闲"；指定不存在的桩 → "不存在或已下线"。
        page->setProperty("phase", QStringLiteral("idle"));
        QMetaObject::invokeMethod(page, "scanCode", Q_ARG(QVariant, QStringLiteral("2")));
        const QVariantMap busyOnly{
            {QStringLiteral("id"), 2LL},
            {QStringLiteral("name"), QStringLiteral("深大北门超充站")},
            {QStringLiteral("chargers"), QVariantList{
                QVariantMap{{QStringLiteral("id"), 21}, {QStringLiteral("status"), QStringLiteral("charging")}}}}};
        query.emitDetail(busyOnly);
        QCOMPARE(page->property("phase").toString(), QStringLiteral("miss"));
        QVERIFY(page->property("failMessage").toString().contains(
            QStringLiteral("暂无空闲")));

        page->setProperty("phase", QStringLiteral("idle"));
        QMetaObject::invokeMethod(page, "scanCode", Q_ARG(QVariant, QStringLiteral("CHG://2/99")));
        query.emitDetail(busyOnly);
        QCOMPARE(page->property("phase").toString(), QStringLiteral("miss"));
        QVERIFY(page->property("failMessage").toString().contains(
            QStringLiteral("不存在")));
    }

    // 批次F 真桥端到端：真 StationQueryService(mock 站表) + 真 StationQueryBridge
    // 挂 ScanPage——入场自拉 6 站，速选科技园站命中空闲桩并组成预约 arg。
    void scanPageEndToEndOnMockStationChannel()
    {
        QmlApp app;
        QVERIFY(app.login(QStringLiteral("13800138000")));
        app.clearUnfinishedOrdersForTesting();
        charging::client::services::station::StationQueryService service;
        charging::qml::StationQueryBridge bridge(&service);
        FakeVehicleBridge vehicles;
        QObject reservations;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        engine.rootContext()->setContextProperty(QStringLiteral("stationQueryService"), &bridge);
        engine.rootContext()->setContextProperty(QStringLiteral("settingsService"), &vehicles);
        engine.rootContext()->setContextProperty(QStringLiteral("reservationService"),
                                                 &reservations);
        QObject holder;
        auto* page = createPage(engine, QStringLiteral("ScanPage.qml"), &holder);
        QVERIFY(page);
        QTRY_COMPARE_WITH_TIMEOUT(
            page->property("stations").toList().size(), 6, 4000);

        QMetaObject::invokeMethod(page, "scanStation", Q_ARG(QVariant, 0));  // 站1：3 空闲
        QTRY_COMPARE_WITH_TIMEOUT(page->property("phase").toString(),
                                  QStringLiteral("found"), 4000);
        QCOMPARE(page->property("foundCharger").toMap().value(QStringLiteral("status")).toString(),
                 QStringLiteral("available"));
        QSignalSpy routes(&app, &QmlApp::navigateRequested);
        QMetaObject::invokeMethod(page, "reserveNow");
        const QVariantMap arg = page->property("reserveArg").toMap();
        QCOMPARE(arg.value(QStringLiteral("stationId")).toString(), QStringLiteral("1"));
        QCOMPARE(arg.value(QStringLiteral("stationName")).toString(),
                 QStringLiteral("科技园充电驿站"));
        QVERIFY(!arg.value(QStringLiteral("chargerCode")).toString().isEmpty());
        QVERIFY(arg.value(QStringLiteral("hasStationLocation")).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(routes.size(), 1, 4000);
        QCOMPARE(routes.first().at(0).toString(), QStringLiteral("reservation_confirm"));
    }
};

QTEST_MAIN(QmlClientPagesTest)

#include "tst_qml_client_pages.moc"
