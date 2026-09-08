// test_qml_station_interactions — station 域真机点击链回归（2026-09-07 用户实测两报）。
//
// 与既有 qml_client_pages 的分工：那边用"信号发射=点击"绕开 offscreen delegate
// 时序问题驱动页面状态；本文件专测**事件投递本身**——真实鼠标事件经
// itemsAtPosition 命中栈送达，钉死两类只在真点击下暴露的缺陷：
//   ① ClickableCard 盖层 MouseArea 吞掉卡内子 MouseArea（收藏 ☆ 点不动，
//      点击被卡面 onClicked 劫持去详情页）；
//   ② 顶栏铃铛 → App.navigate("notifications") → Shell 翻页全链（消息页进不去），
//      以及"我的"页消息通知入口行（修复前该入口根本不存在）；
//   ③ 找站页重设计 map⇄list 分段 + peek 浮卡：selectedMarker=-1 越界守卫；
//   ④ 2026-09-08 批量指令①：车辆强制校验撤除——0 车（无充电中）点"预约"直达
//      确认页；chargingBusy=true 时 chargingBusyPrompt 拦截且不导航。
//      chargingBusy 两态由属性直注（mock 服务数据是否含充电中单不作保），
//      钉的是 UI 拦截分支本身，服务侧名额语义归 reservation_service 测试。
//   ⑤ 2026-09-08 用户实测：导航页【更换】弹窗打开即冻结——裸宽 Column ×
//      子项 parent.height - y 自回边 → QQuickItem::polish() loop 每帧刷。
//      修复=Column anchors.fill 显式定高；本例真点【更换】+message handler
//      计数钉"零 loop 行 + 弹层有行"。
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

#include <atomic>
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

// 递归收集 objectName 匹配的全部 item（桩列表里每卡一个预约按钮）。
void collectItems(QQuickItem* root, const QString& objectName, QList<QQuickItem*>& out)
{
    if (root == nullptr)
        return;
    for (QQuickItem* child : root->childItems()) {
        if (child->objectName() == objectName)
            out.append(child);
        collectItems(child, objectName, out);
    }
}

// polish() loop 行数计数——QtMessageHandler 是函数指针，lambda 不可带捕获，
// 放文件级（仅 navigationPickPopupOpensWithoutPolishLoop 安装期间有流量）。
std::atomic<int> g_polishLoops{0};
void countPolishLoopHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    if (msg.contains(QStringLiteral("polish() loop")))
        ++g_polishLoops;
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

    // 公共链：station 卡文字区真点击 → 详情页（列表 mock + 翻页 + 桩 mock 各留余量）。
    QQuickItem* enterDetailPage()
    {
        auto* card = findItem(window_->contentItem(), QStringLiteral("stationCard"));
        if (card == nullptr) {   // offscreen delegate 惰性：先强制一次场景图渲染
            auto shot = window_->contentItem()->grabToImage();
            QEventLoop loop;
            QObject::connect(shot.get(), &QQuickItemGrabResult::ready,
                             &loop, &QEventLoop::quit);
            loop.exec();
            spin(50);
            card = findItem(window_->contentItem(), QStringLiteral("stationCard"));
        }
        if (card == nullptr)
            return nullptr;
        QQuickItem* title = nullptr;
        std::function<void(QQuickItem*)> walkText = [&](QQuickItem* it) {
            if (title) return;
            const QString t = it->property("text").toString();
            if (t.length() >= 4 && t != QStringLiteral("营业中")) { title = it; return; }
            for (QQuickItem* c : it->childItems()) walkText(c);
        };
        walkText(card);
        if (title == nullptr)
            return nullptr;
        realClick(window_, title);
        spin(400);
        auto* detail = findItem(window_->contentItem(), QStringLiteral("stationDetailPage"));
        if (detail == nullptr)
            return nullptr;
        spin(900);   // 桩列表 mock 落定 + delegate 实例化
        return detail;
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

    // 批量指令① 正断言：车辆校验整套撤除——chargingBusy=false（无充电中）时，
    // 详情页点"预约"直达 reservationConfirmPage；拦截弹层不得出现
    //（旧 vehicleRequiredPrompt/unfinishedReservationPrompt 链已删，弹层不实例化）。
    void zeroVehicleNoChargingReservesStraightToConfirm()
    {
        bootShell(QStringLiteral("station"));
        auto* detail = enterDetailPage();
        QVERIFY2(detail != nullptr, "卡片文字区点击未进详情页");
        QVERIFY(detail->setProperty("chargingBusy", false));
        QList<QQuickItem*> buttons;
        collectItems(detail, QStringLiteral("detailReserveButton"), buttons);
        QQuickItem* target = nullptr;
        for (auto* b : buttons)
            if (b->isEnabled()) { target = b; break; }
        QVERIFY2(target != nullptr, "无 available 桩的启用预约按钮（mock 桩数据变了？）");
        realClick(window_, target);
        spin(400);
        QVERIFY2(findItem(window_->contentItem(), QStringLiteral("reservationConfirmPage"))
                     != nullptr,
                 "0 车点预约未直达确认页（旧车辆闸回归？）");
        auto* go = findItem(window_->contentItem(), QStringLiteral("chargingBusyGoButton"));
        QVERIFY2(go == nullptr || !go->isVisible(), "无充电中却弹出充电拦截浮层");
    }

    // 批量指令① 反断言：chargingBusy=true（有车辆正在充电）→ 预约必须被
    // chargingBusyPrompt 拦截且不导航（新业务唯一保留闸）。
    void chargingVehicleBlocksReserveWithPrompt()
    {
        bootShell(QStringLiteral("station"));
        auto* detail = enterDetailPage();
        QVERIFY2(detail != nullptr, "卡片文字区点击未进详情页");
        QVERIFY(detail->setProperty("chargingBusy", true));
        QList<QQuickItem*> buttons;
        collectItems(detail, QStringLiteral("detailReserveButton"), buttons);
        QQuickItem* target = nullptr;
        for (auto* b : buttons)
            if (b->isEnabled()) { target = b; break; }
        QVERIFY2(target != nullptr, "无 available 桩的启用预约按钮");
        realClick(window_, target);
        spin(300);
        QVERIFY2(findItem(window_->contentItem(), QStringLiteral("reservationConfirmPage"))
                     == nullptr,
                 "有车辆充电中竟发起了预约（拦截闸失效）");
        auto* go = findItem(window_->contentItem(), QStringLiteral("chargingBusyGoButton"));
        QVERIFY2(go != nullptr && go->isVisible(), "chargingBusyPrompt 拦截弹层未出现");
    }

    // 批量指令③+实测冻结回归：导航页 destinationRow【更换】真点击 →
    // navigationPickPopup 打开并出候选行；期间 QQuickItem::polish() loop 计数
    // 必须为 0（修复前该弹窗 Column 无显式高，子项 height:parent.height-y
    // 自回边，每帧刷 loop 告警直至 UI 冻结——offscreen 同样复现）。
    void navigationPickPopupOpensWithoutPolishLoop()
    {
        bootShell(QStringLiteral("station"));
        QVariantMap record;
        record.insert(QStringLiteral("stationName"), QStringLiteral("测试充电站"));
        record.insert(QStringLiteral("stationAddress"), QStringLiteral("南山区测试路 1 号"));
        record.insert(QStringLiteral("stationLatitude"), 22.52);
        record.insert(QStringLiteral("stationLongitude"), 113.95);
        record.insert(QStringLiteral("hasStationLocation"), true);
        record.insert(QStringLiteral("distanceMeters"), 3200);
        QMetaObject::invokeMethod(app_, "navigate",
                                  Q_ARG(QString, QStringLiteral("navigation")),
                                  Q_ARG(QVariant, QVariant(record)));
        spin(600);
        auto* change = findItem(window_->contentItem(),
                                QStringLiteral("destinationChangeButton"));
        QVERIFY2(change != nullptr, "导航页未出现 destinationChangeButton（③改动丢失？）");

        g_polishLoops = 0;
        auto* prev = qInstallMessageHandler(countPolishLoopHandler);
        realClick(window_, change);
        spin(900);   // 弹层打开 + mock 全量检索回填 + 多帧布局（修复前此处已刷千行 loop）
        qInstallMessageHandler(prev);
        QVERIFY2(g_polishLoops.load() == 0,
                 qPrintable(QStringLiteral("【更换】弹层触发 %1 条 polish() loop（自回边回归）")
                            .arg(g_polishLoops.load())));
        auto* list = findItem(window_->contentItem(), QStringLiteral("navigationPickList"));
        QVERIFY2(list != nullptr, "弹层内容层未实例化（Popup 未打开？）");
        QVERIFY2(!list->childItems().isEmpty(), "弹层无候选行（检索/兜底链断了？）");
    }
};

QTEST_MAIN(QmlStationInteractionsTest)
#include "tst_qml_station_interactions.moc"
