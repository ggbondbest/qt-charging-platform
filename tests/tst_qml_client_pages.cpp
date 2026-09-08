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

#include "charging/client/profile_charging/avatar_library.h"

#include "app_bridge.h"
#include "service_bridges.h"

using charging::qml::ChargingBridge;
using charging::qml::CouponBridge;
using charging::qml::OrderBridge;
using charging::qml::PointBridge;
using charging::qml::QmlApp;
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
    Q_INVOKABLE void updateNickname(const QString& nickname)
    {
        calls << (QStringLiteral("nick:") + nickname);
    }
    Q_INVOKABLE void updateAvatar(const QString& avatarKey)
    {
        calls << (QStringLiteral("avatar:") + avatarKey);
    }

    void emitProfileLoaded() { emit profileLoaded(QVariantMap{}); }
    void emitFailure()
    {
        emit operationFailed(QStringLiteral("UPDATE_USER_INFO"), QStringLiteral("MOCK"),
                             QStringLiteral("模拟资料保存失败"));
    }

signals:
    void profileLoaded(const QVariantMap& user);
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

} // namespace

class QmlClientPagesTest final : public QObject
{
    Q_OBJECT

    QQuickWindow* window_ = nullptr;

private slots:
    void init()
    {
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

        fake.emitProfileLoaded(); // 昵称落定 → 自动补发头像
        QCOMPARE(fake.calls.size(), 2);
        QCOMPARE(fake.calls.at(1), QStringLiteral("avatar:cat"));
        QCOMPARE(backSpy.count(), 0); // 原 bug：第一步成功就退出，头像丢失

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
};

QTEST_MAIN(QmlClientPagesTest)

#include "tst_qml_client_pages.moc"
