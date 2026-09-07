// test_qml_station_interactions — station 域真机点击链回归（2026-09-07 用户实测两报）。
//
// 与既有 qml_client_pages 的分工：那边用"信号发射=点击"绕开 offscreen delegate
// 时序问题驱动页面状态；本文件专测**事件投递本身**——真实鼠标事件经
// itemsAtPosition 命中栈送达，钉死两类只在真点击下暴露的缺陷：
//   ① ClickableCard 盖层 MouseArea 吞掉卡内子 MouseArea（收藏 ☆ 点不动，
//      点击被卡面 onClicked 劫持去详情页）；
//   ② 顶栏铃铛 → App.navigate("notifications") → Shell 翻页全链（消息页进不去），
//      以及"我的"页消息通知入口行（修复前该入口根本不存在）；
//   ③ 找站页重设计 map⇄list 分段 + peek 浮卡：selectedMarker=-1 越界守卫。
//
// 宿主与 charging-qml-preview 同构：QmlApp + 全量契约 context property +
// Shell.qml file:// 加载（offscreen 可跑，QTEST_MAIN 自带 QGuiApplication）。
#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QTimer>

#include <functional>
#include <memory>

#include "app_bridge.h"

using charging::qml::QmlApp;

namespace {

// 递归找第一个 objectName 匹配的 item（StackView 页面/顶栏子树均在 contentItem 下）。
QQuickItem* findItem(QQuickItem* root, const QString& objectName)
{
    if (root == nullptr)
        return nullptr;
    for (QQuickItem* child : root->childItems()) {
        if (child->objectName() == objectName)
            return child;
        if (QQuickItem* hit = findItem(child, objectName))
            return hit;
    }
    return nullptr;
}

// 真实鼠标点击：mapToScene 取中心 → 经窗口事件系统投递（含命中栈穿透）。
void realClick(QQuickWindow* window, QQuickItem* item)
{
    QVERIFY(item != nullptr);
    const QPointF center = item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
}

void spin(int msec)
{
    QTest::qWait(msec);
}

} // namespace

class QmlStationInteractionsTest final : public QObject
{
    Q_OBJECT

    QQuickWindow* window_ = nullptr;
    QQmlEngine* engine_ = nullptr;
    QmlApp* app_ = nullptr;

    [[maybe_unused]] void bootShell(const QString& view)
    {
        engine_ = new QQmlEngine;
        app_ = new QmlApp;
        auto* ctx = engine_->rootContext();
        ctx->setContextProperty(QStringLiteral("chargingView"), view);
        ctx->setContextProperty(QStringLiteral("chargingArg"), QVariant());
        ctx->setContextProperty(QStringLiteral("App"), app_);
        ctx->setContextProperty(QStringLiteral("CHARGING_CHANNEL"), QStringLiteral("mock"));
        app_->login(QStringLiteral("13800138000"));
        ctx->setContextProperty(QStringLiteral("walletService"), app_->walletService());
        ctx->setContextProperty(QStringLiteral("orderService"), app_->orderService());
        ctx->setContextProperty(QStringLiteral("chargingService"), app_->chargingService());
        ctx->setContextProperty(QStringLiteral("reservationService"), app_->reservationService());
        ctx->setContextProperty(QStringLiteral("settingsService"), app_->settingsService());
        ctx->setContextProperty(QStringLiteral("mapGeoService"), app_->mapGeoService());
        ctx->setContextProperty(QStringLiteral("favoritesService"), app_->favoritesService());
        ctx->setContextProperty(QStringLiteral("notificationService"), app_->notificationService());
        ctx->setContextProperty(QStringLiteral("stationQueryService"), app_->stationQueryService());
        ctx->setContextProperty(QStringLiteral("authService"), app_->authService());

        QQmlComponent component(engine_);
        component.loadUrl(QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
                                              + QStringLiteral("/Shell.qml")));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        window_ = qobject_cast<QQuickWindow*>(component.create());
        QVERIFY(window_ != nullptr);
        window_->resize(420, 860);
        window_->show();
        // 首屏 mock 查询 450ms + 二次布局余量。
        spin(900);
    }

    void teardownShell()
    {
        delete window_;
        window_ = nullptr;
        delete engine_;
        engine_ = nullptr;
        delete app_;
        app_ = nullptr;
    }

private slots:
    void cleanup() { teardownShell(); }

    // 用户报①"消息页面进不去"：station 页真点顶栏铃铛 → notificationPage 上屏。
    void bellOpensNotificationPage()
    {
        bootShell(QStringLiteral("station"));
        auto* nav = findItem(window_->contentItem(), QStringLiteral("topNavBar"));
        QVERIFY(nav != nullptr);
        // 铃铛 = 无 objectName 的 Text("🔔")；从顶栏子树按 text 找。
        QQuickItem* bell = nullptr;
        std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {
            if (bell) return;
            if (it->property("text").toString() == QStringLiteral("🔔")) { bell = it; return; }
            for (QQuickItem* c : it->childItems()) walk(c);
        };
        walk(nav);
        QVERIFY2(bell != nullptr, "顶栏铃铛未渲染（station 页 searchVisible 联动失效？）");
        QVERIFY2(bell->isVisible(), "铃铛 visible=false");
        realClick(window_, bell);
        spin(300);
        auto* page = findItem(window_->contentItem(), QStringLiteral("notificationPage"));
        QVERIFY2(page != nullptr, "点击铃铛后消息页未上屏");
        QVERIFY(page->isVisible());
    }

    // 用户报②"收藏按钮点不了"：station 卡片右下角 ☆ 真点击 = 收藏切换，
    // 且**不得**把点击漏给卡面（现状：ClickableCard 盖层吞事件 → 跳详情页）。
    void stationStarClickTogglesFavorite()
    {
        bootShell(QStringLiteral("station"));
        auto* card = findItem(window_->contentItem(), QStringLiteral("stationCard"));
        if (card == nullptr) { // offscreen delegate 惰性：强制一次场景图渲染再找
            auto shot = window_->contentItem()->grabToImage();
            QEventLoop loop;
            QObject::connect(shot.get(), &QQuickItemGrabResult::ready,
                             &loop, &QEventLoop::quit);
            loop.exec();
            spin(50);
            card = findItem(window_->contentItem(), QStringLiteral("stationCard"));
        }
        QVERIFY2(card != nullptr, "stationCard delegate 未实例化（列表没渲染？）");
        auto* star = findItem(card, QStringLiteral("favoriteStarButton"));
        QVERIFY2(star != nullptr, "favoriteStarButton 未渲染");
        // 星 glyph（delegate 绑定 page.isFav(stationId)，role 属性 C++ 侧拿不到，
        // 以字形翻转判定收藏态翻转）。
        auto starGlyph = [&](QQuickItem* starItem) {
            for (QQuickItem* c : starItem->childItems()) {
                const QString t = c->property("text").toString();
                if (t == QStringLiteral("★") || t == QStringLiteral("☆"))
                    return t;
            }
            return QString();
        };
        const QString before = starGlyph(star);
        QVERIFY2(before == QStringLiteral("★") || before == QStringLiteral("☆"),
                 "favoriteStarButton 内未找到星字形 Text");
        realClick(window_, star);
        spin(200);
        // 收藏翻转：onFavoritesChanged→project() 重建 delegate，重新取星
        auto* card2 = findItem(window_->contentItem(), QStringLiteral("stationCard"));
        auto* star2 = card2 ? findItem(card2, QStringLiteral("favoriteStarButton")) : nullptr;
        QVERIFY2(card2 != nullptr, "收藏后星按钮丢失");
        // 先钉"没被卡面吞走"（StackView push 后旧页仍在树里，故以详情页在否判劫持）
        QVERIFY2(findItem(window_->contentItem(), QStringLiteral("stationDetailPage")) == nullptr,
                 "点击收藏星后被卡面导航劫持进详情页（事件被盖层吞掉）");
        QCOMPARE(starGlyph(star2), before == QStringLiteral("★")
                                        ? QStringLiteral("☆") : QStringLiteral("★"));
        // 反向：卡面文字区点击仍走卡导航（盖层下沉路径不能被本修复打断）
        QQuickItem* title = nullptr;
        std::function<void(QQuickItem*)> walkText = [&](QQuickItem* it) {
            if (title) return;
            const QString t = it->property("text").toString();
            // DFS 先序到达的长文本 = 卡内站点名行（无自身 handler，在盖层之下）
            if (t.length() >= 4 && t != QStringLiteral("营业中")) {
                title = it;
                return;
            }
            for (QQuickItem* c : it->childItems()) walkText(c);
        };
        walkText(card2);
        QVERIFY(title != nullptr);
        realClick(window_, title);
        spin(300);
        QVERIFY2(findItem(window_->contentItem(), QStringLiteral("stationDetailPage")) != nullptr,
                 "卡面文字区点击丢失导航（盖层下沉链被破坏）");
    }

    // 用户报①的另一入口：profile"消息通知"行真点击 → 消息页上屏
    //（修复前 profile 没有该入口，消息页从"我的"完全不可达即此）。
    void profileNotificationsRowOpensPage()
    {
        bootShell(QStringLiteral("profile"));
        auto* row = findItem(window_->contentItem(), QStringLiteral("openNotificationsButton"));
        QVERIFY2(row != nullptr, "profile 消息通知入口行缺失");
        realClick(window_, row);
        spin(300);
        QVERIFY2(findItem(window_->contentItem(), QStringLiteral("notificationPage")) != nullptr,
                 "点击消息通知行后消息页未上屏");
    }

    // 找站页重设计（map⇄list 分段 + peek 浮卡）回归钉：未选中时 peek 必须不浮现
    //（selectedMarker=-1 越界崩溃守卫），选中后信息绑定 + "详情"导航链完好。
    void mapSegmentPeekCardShowsSelectedStation()
    {
        bootShell(QStringLiteral("station"));
        auto* seg = findItem(window_->contentItem(), QStringLiteral("viewModeMapButton"));
        QVERIFY2(seg != nullptr, "map⇄list 分段按钮未渲染");
        realClick(window_, seg);
        spin(200);
        auto* peek = findItem(window_->contentItem(), QStringLiteral("stationPeekCard"));
        QVERIFY2(peek != nullptr, "peek 浮卡未实例化");
        QVERIFY2(!peek->isVisible(), "未选中站点时 peek 不应浮现（-1 越界守卫失效）");
        auto* page = findItem(window_->contentItem(), QStringLiteral("stationHomePage"));
        QVERIFY(page != nullptr);
        QVERIFY(page->setProperty("selectedMarker", 0));
        spin(200);
        QVERIFY2(peek->isVisible(), "选中站点后 peek 浮卡未出现");
        bool hasName = false;
        std::function<void(QQuickItem*)> walkPeek = [&](QQuickItem* it) {
            if (hasName) return;
            if (it->property("text").toString().length() >= 4) { hasName = true; return; }
            for (QQuickItem* c : it->childItems()) walkPeek(c);
        };
        walkPeek(peek);
        QVERIFY2(hasName, "peek 卡信息绑定为空（越界守卫回归？）");
        auto* open = findItem(peek, QStringLiteral("stationPeekOpenButton"));
        QVERIFY(open != nullptr);
        realClick(window_, open);
        spin(300);
        QVERIFY2(findItem(window_->contentItem(), QStringLiteral("stationDetailPage")) != nullptr,
                 "peek 详情按钮未进入站点详情");
    }
};

QTEST_MAIN(QmlStationInteractionsTest)
#include "tst_qml_station_interactions.moc"
