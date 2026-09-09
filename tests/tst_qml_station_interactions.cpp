// Mouse-event regression for the delivered address -> map -> station UI.
// App uses MockRequestTransport; all map credentials and preferences are isolated.
#include <QtTest>
#include <QJSValue>
#include <QDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "app_bridge.h"
#include "map_bridge.h"
#include "service_bridges.h"
#include "charging/client/profile_charging/progress_service.h"

using charging::qml::QmlApp;
using charging::qml::MapBridge;
using charging::qml::SettingsBridge;

namespace {
QQuickItem* findItem(QQuickItem* root, const QString& name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems()) {
        if (auto* found = findItem(child, name)) return found;
    }
    return nullptr;
}
QQuickItem* findText(QQuickItem* root, const QString& text)
{
    if (!root) return nullptr;
    if (root->property("text").toString() == text) return root;
    for (auto* child : root->childItems()) {
        if (auto* found = findText(child, text)) return found;
    }
    return nullptr;
}
QVariant plainVariant(const QVariant& value)
{
    return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
}
bool realClick(QQuickWindow* window, QQuickItem* item)
{
    if (!window || !item || !item->isVisible() || !item->isEnabled()) return false;
    const auto center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    if (!QRectF(0, 0, window->width(), window->height()).contains(center)) return false;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    return true;
}
} // namespace

class QmlStationInteractionsTest final : public QObject
{
    Q_OBJECT
    QQuickWindow* window_ = nullptr;
    QQmlEngine* engine_ = nullptr;
    QmlApp* app_ = nullptr;
    QTemporaryDir preferences_;
    QStringList qmlWarnings_;

    void bootShell(const QString& view = QStringLiteral("station"), int width = 420)
    {
        qmlWarnings_.clear();
        engine_ = new QQmlEngine;
        // A page can still render and accept clicks after a binding fails.
        // Treat engine warnings as failures, just like the packaged UI smoke.
        connect(engine_, &QQmlEngine::warnings, this, [this](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) qmlWarnings_.append(warning.toString());
        });
        app_ = new QmlApp(QStringLiteral("127.0.0.1"), 9527, true);
        qobject_cast<MapBridge*>(app_->mapBridge())->setBrowsingCity(QStringLiteral("深圳市"));
        app_->login(QStringLiteral("13800138000"));
        QTRY_VERIFY(app_->loggedIn());
        auto* settings = qobject_cast<SettingsBridge*>(app_->settingsService());
        QVERIFY(settings != nullptr);
        settings->setTheme(QStringLiteral("light"));
        settings->setFontScale(QStringLiteral("standard"));
        auto* ctx = engine_->rootContext();
        ctx->setContextProperty(QStringLiteral("chargingView"), view);
        ctx->setContextProperty(QStringLiteral("chargingArg"), QVariant());
        ctx->setContextProperty(QStringLiteral("App"), app_);
        ctx->setContextProperty(QStringLiteral("CHARGING_CHANNEL"), QStringLiteral("mock"));
        ctx->setContextProperty(QStringLiteral("walletService"), app_->walletService());
        ctx->setContextProperty(QStringLiteral("orderService"), app_->orderService());
        ctx->setContextProperty(QStringLiteral("chargingService"), app_->chargingService());
        ctx->setContextProperty(QStringLiteral("reservationService"), app_->reservationService());
        ctx->setContextProperty(QStringLiteral("settingsService"), app_->settingsService());
        ctx->setContextProperty(QStringLiteral("mapGeoService"), app_->mapGeoService());
        ctx->setContextProperty(QStringLiteral("mapBridge"), app_->mapBridge());
        ctx->setContextProperty(QStringLiteral("favoritesService"), app_->favoritesService());
        ctx->setContextProperty(QStringLiteral("notificationService"), app_->notificationService());
        ctx->setContextProperty(QStringLiteral("stationQueryService"), app_->stationQueryService());
        ctx->setContextProperty(QStringLiteral("statsService"), app_->statsService());
        ctx->setContextProperty(QStringLiteral("couponService"), app_->couponService());
        ctx->setContextProperty(QStringLiteral("pointsService"), app_->pointsService());
        ctx->setContextProperty(QStringLiteral("ratingsService"), app_->ratingsService());
        ctx->setContextProperty(QStringLiteral("authService"), app_->authService());
        QQmlComponent component(engine_);
        component.loadUrl(QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR) + "/Shell.qml"));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        window_ = qobject_cast<QQuickWindow*>(component.create());
        QVERIFY2(window_ != nullptr, qPrintable(component.errorString()));
        window_->resize(width, 860);
        window_->show();
        QTest::qWait(100);
        if (view == QStringLiteral("station")) {
            auto* home = findItem(window_->contentItem(), QStringLiteral("stationHomePage"));
            QVERIFY(home != nullptr);
            QTRY_VERIFY(home->property("loaded").toBool());
            QTRY_VERIFY(!plainVariant(home->property("raw")).toList().isEmpty());
        }
    }

    QQuickItem* visibleStationCard()
    {
        auto* list = findItem(window_->contentItem(), QStringLiteral("stationList"));
        if (!list) return nullptr;
        // Header contains the map: instantiate AND scroll the delegate before clicking.
        if (!QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 0), Q_ARG(int, 0)))
            return nullptr;
        QTest::qWait(100);
        return findItem(list, QStringLiteral("stationCard"));
    }

    QQuickItem* enterDetailPage()
    {
        auto* card = visibleStationCard();
        if (!card || !realClick(window_, findItem(card, QStringLiteral("stationReserveButton")))) return nullptr;
        for (int attempt = 0; attempt < 40; ++attempt) {
            QTest::qWait(50);
            auto* detail = findItem(window_->contentItem(), QStringLiteral("stationDetailPage"));
            auto* stack = findItem(window_->contentItem(), QStringLiteral("pageStack"));
            if (detail && detail->isVisible() && detail->property("detailLoaded").toBool()
                && stack && !stack->property("busy").toBool()) {
                // A completed service callback does not imply the opacity
                // animation has finished. Click after the scene has settled.
                QTest::qWait(200);
                return detail;
            }
        }
        return nullptr;
    }

    QQuickItem* availableReserveButton(QQuickItem* detail)
    {
        QList<QQuickItem*> pending{detail};
        while (!pending.isEmpty()) {
            auto* item = pending.takeFirst();
            if (item->objectName() == QStringLiteral("detailReserveButton") && item->isEnabled()) return item;
            pending.append(item->childItems());
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(preferences_.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatform.Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("StationInteractions"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences_.path());
        for (const char* name : {"TENCENT_MAP_API_KEY", "TENCENT_MAP_SECRET_KEY", "TENCENT_MAP_JS_KEY",
                                 "CHARGING_TENCENT_MAP_KEY", "CHARGING_TENCENT_MAP_SECRET"}) qputenv(name, "");
        qputenv("CHARGING_CHANNEL", "mock");
    }

    void cleanup()
    {
        delete window_; window_ = nullptr;
        delete engine_; engine_ = nullptr;
        delete app_; app_ = nullptr;
        QVERIFY2(qmlWarnings_.isEmpty(), qPrintable(qmlWarnings_.join(QLatin1Char('\n'))));
    }

    void bellOpensNotificationPage()
    {
        bootShell();
        QVERIFY(window_ != nullptr);
        auto* bell = findText(findItem(window_->contentItem(), QStringLiteral("topNavBar")), QStringLiteral("🔔"));
        QVERIFY(realClick(window_, bell));
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("notificationPage")) != nullptr);
    }

    void stationStarClickTogglesFavoriteWithoutOpeningDetails()
    {
        bootShell();
        QVERIFY(window_ != nullptr);
        auto* card = visibleStationCard();
        QVERIFY(card != nullptr);
        auto* star = findItem(card, QStringLiteral("favoriteStarButton"));
        QVERIFY(star != nullptr);
        const QString before = star->property("text").toString();
        QVERIFY(before == QStringLiteral("☆") || before == QStringLiteral("★"));
        QVERIFY(realClick(window_, star));
        QTest::qWait(150);
        card = visibleStationCard();
        QVERIFY(card != nullptr);
        star = findItem(card, QStringLiteral("favoriteStarButton"));
        QVERIFY(star != nullptr);
        QCOMPARE(star->property("text").toString(), before == QStringLiteral("☆") ? QStringLiteral("★") : QStringLiteral("☆"));
        QVERIFY(findItem(window_->contentItem(), QStringLiteral("stationDetailPage")) == nullptr);
        // Card surface still navigates after correcting nested button events.
        QVERIFY(realClick(window_, card));
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("stationDetailPage")) != nullptr);
    }

    void profileNotificationsRowOpensPage()
    {
        bootShell(QStringLiteral("profile"));
        QVERIFY(window_ != nullptr);
        QVERIFY(realClick(window_, findItem(window_->contentItem(), QStringLiteral("openNotificationsButton"))));
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("notificationPage")) != nullptr);
    }

    void cardPriceStaysInsideCard_data()
    {
        QTest::addColumn<int>("windowWidth");
        QTest::newRow("standard") << 420;
        QTest::newRow("narrow") << 360;
    }
    void cardPriceStaysInsideCard()
    {
        QFETCH(int, windowWidth);
        bootShell(QStringLiteral("station"), windowWidth);
        QVERIFY(window_ != nullptr);
        auto* card = visibleStationCard();
        QVERIFY(card != nullptr);
        auto* price = findItem(card, QStringLiteral("stationPrice"));
        QVERIFY(price != nullptr);
        const QRectF priceRect(price->mapToItem(card, QPointF()), QSizeF(price->width(), price->height()));
        QVERIFY2(QRectF(0, 0, card->width(), card->height()).contains(priceRect), "电价溢出站点卡片");
        QVERIFY(price->property("text").toString().startsWith(QStringLiteral("¥")));
        for (const QString& name : {QStringLiteral("favoriteStarButton"), QStringLiteral("stationNavigateButton"),
                                    QStringLiteral("stationReserveButton")}) {
            auto* action = findItem(card, name);
            QVERIFY(action != nullptr);
            QVERIFY(QRectF(0, 0, card->width(), card->height()).contains(
                QRectF(action->mapToItem(card, QPointF()), QSizeF(action->width(), action->height()))));
        }
        const auto directory = qEnvironmentVariable("CHARGING_INTERACTION_SCREENSHOTS");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(window_->grabWindow().save(QDir(directory).filePath(
                QStringLiteral("station-card-%1.png").arg(windowWidth))));
        }
    }

    void mapSelectionRejectsUnknownIdAndOpensMatchingStation_data()
    {
        QTest::addColumn<int>("windowWidth");
        QTest::newRow("standard") << 420;
        QTest::newRow("narrow") << 360;
    }

    void mapSelectionRejectsUnknownIdAndOpensMatchingStation()
    {
        QFETCH(int, windowWidth);
        bootShell(QStringLiteral("station"), windowWidth);
        QVERIFY(window_ != nullptr);
        auto* page = findItem(window_->contentItem(), QStringLiteral("stationHomePage"));
        QVERIFY(page != nullptr);
        QSignalSpy navigated(app_, &QmlApp::navigateRequested);
        QVERIFY(QMetaObject::invokeMethod(page, "selectMapStation", Q_ARG(QVariant, QStringLiteral("nonexistent-id"))));
        QVERIFY(plainVariant(page->property("selectedStation")).toMap().isEmpty());
        QCOMPARE(navigated.size(), 0);
        const auto records = plainVariant(page->property("raw")).toList();
        QVERIFY(!records.isEmpty());
        const QString id = records.first().toMap().value(QStringLiteral("id")).toString();
        QVERIFY(QMetaObject::invokeMethod(page, "selectMapStation", Q_ARG(QVariant, id)));
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("mapStationReserveButton")) != nullptr);
        auto* popup = page->findChild<QObject*>(QStringLiteral("mapStationPopup"));
        QVERIFY(popup != nullptr);
        auto* overlay = popup->property("parent").value<QQuickItem*>();
        QVERIFY(overlay != nullptr);
        QVERIFY(overlay != page);
        QCOMPARE(overlay->window(), window_);
        QCOMPARE(overlay->width(), qreal(window_->width()));
        QCOMPARE(overlay->height(), qreal(window_->height()));
        QTRY_VERIFY(popup->property("opened").toBool());
        QCOMPARE(plainVariant(page->property("selectedStation")).toMap().value(QStringLiteral("stationId")).toString(), id);
        auto* reserve = findItem(window_->contentItem(), QStringLiteral("mapStationReserveButton"));
        QTest::qWait(150);
        QVERIFY(realClick(window_, reserve));
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("stationDetailPage")) != nullptr);
        QCOMPARE(navigated.last().at(0).toString(), QStringLiteral("station_detail"));
        QCOMPARE(navigated.last().at(1).toMap().value(QStringLiteral("id")).toString(), id);
    }

    void missingKeyDoesNotLeaveSecondManualOriginRequestBusy()
    {
        bootShell();
        QVERIFY(window_ != nullptr);
        auto* bridge = qobject_cast<MapBridge*>(app_->mapBridge());
        QVERIFY(bridge != nullptr);
        auto* input = findItem(window_->contentItem(), QStringLiteral("originAddressField"));
        auto* locate = findItem(window_->contentItem(), QStringLiteral("locateAddressButton"));
        QVERIFY(input && locate);
        QSignalSpy changed(bridge, &MapBridge::changed);
        for (const QString& address : {QStringLiteral("软件园路"), QStringLiteral("黄浦路"), QStringLiteral("软件园路")}) {
            input->setProperty("text", address);
            const int before = changed.size();
            QVERIFY(realClick(window_, locate));
            QTRY_VERIFY(changed.size() > before);
            QTRY_VERIFY(!bridge->busy() && !bridge->error().isEmpty());
            QVERIFY(!bridge->hasLocation());
            QVERIFY(bridge->routeHtml().isEmpty());
            QVERIFY(locate->isEnabled());
        }
    }

    void zeroVehicleNoUnfinishedOrderCanReserve()
    {
        bootShell();
        QVERIFY(window_ != nullptr);
        auto* settings = qobject_cast<SettingsBridge*>(app_->settingsService());
        QVERIFY(settings != nullptr);
        for (const auto& vehicle : settings->vehicles()) settings->removeVehicle(vehicle.toMap().value("id"));
        QCOMPARE(settings->vehicleCount(), 0);
        app_->clearUnfinishedOrdersForTesting();
        auto* detail = enterDetailPage();
        QVERIFY(detail != nullptr);
        auto* reserve = availableReserveButton(detail);
        QVERIFY(reserve != nullptr);
        if (qEnvironmentVariableIsSet("CHARGING_INTERACTION_SCREENSHOTS")) {
            const auto directory = qEnvironmentVariable("CHARGING_INTERACTION_SCREENSHOTS");
            QDir().mkpath(directory);
            window_->grabWindow().save(QDir(directory).filePath("detail.png"));
        }
        QSignalSpy clicked(reserve, SIGNAL(clicked()));
        QVERIFY(realClick(window_, reserve));
        QCOMPARE(clicked.size(), 1);
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("reservationConfirmPage")) != nullptr);
    }

    void realMockActiveOrderCheckBlocksSecondReservation()
    {
        bootShell();
        QVERIFY(window_ != nullptr);
        auto* detail = enterDetailPage();
        QVERIFY(detail != nullptr);
        QSignalSpy navigated(app_, &QmlApp::navigateRequested);
        QSignalSpy toast(app_, &QmlApp::toastRequested);
        auto* reserve = availableReserveButton(detail);
        QVERIFY(reserve != nullptr);
        QSignalSpy clicked(reserve, SIGNAL(clicked()));
        QVERIFY(realClick(window_, reserve));
        QCOMPARE(clicked.size(), 1);
        QTRY_VERIFY(!navigated.isEmpty());
        QVERIFY(!app_->checkingOrders());
        QCOMPARE(navigated.last().at(0).toString(), QStringLiteral("charging"));
        QVERIFY(!toast.isEmpty());
        QVERIFY(findItem(window_->contentItem(), QStringLiteral("reservationConfirmPage")) == nullptr);
    }

    // 经验等级批（2026-09-09）真壳端到端 + 会员中心批（同日）改版：等级三件套
    // 从 hero 搬进昵称框与余额框之间的独立「会员等级卡」（uiLevelCard），点卡进
    // 会员中心页（等级阶梯+每日任务区块+礼包记录）；行列表撤任务/等级两行、与
    // 设置并列新增「积分商城」；App.navigate 漏斗与 tasks 深链路由不变。
    void profileLevelBarTasksPageAndXpFunnel()
    {
        bootShell(QStringLiteral("profile"));
        auto* prog = qobject_cast<charging::client::ProgressService*>(app_->progressService());
        QVERIFY(prog);
        QVERIFY(findItem(window_->contentItem(), QStringLiteral("uiLevelCard")) != nullptr);
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("uiLevelBar")) != nullptr);
        auto* xpLabel = findItem(window_->contentItem(), QStringLiteral("uiLevelXpLabel"));
        QVERIFY(xpLabel != nullptr);
        QVERIFY(!xpLabel->property("text").toString().isEmpty());

        QVERIFY(realClick(window_, findItem(window_->contentItem(), QStringLiteral("uiLevelBadgeButton"))));
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("levelPage")) != nullptr);
        // 每日任务已并入会员中心页（TaskSection 直子对象）。
        QVERIFY(findItem(window_->contentItem(), QStringLiteral("uiTaskSection")) != nullptr);

        app_->navigate(QStringLiteral("profile"));    // tab 重进（clear+replace 同款）
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("profilePage")) != nullptr);
        // 商城行恰在 860px 折叠线附近：钉行存在（与设置并列的实据）+ 走路由进页。
        QVERIFY(findItem(window_->contentItem(), QStringLiteral("openMallButton")) != nullptr);
        app_->navigate(QStringLiteral("points_mall"));
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("pointsMallPage")) != nullptr);
        QVERIFY(findItem(window_->contentItem(), QStringLiteral("uiMallTitle")) != nullptr);

        app_->navigate(QStringLiteral("tasks"));      // 独立任务页保留深链
        QTRY_VERIFY(findItem(window_->contentItem(), QStringLiteral("tasksPage")) != nullptr);
        QVERIFY(findItem(window_->contentItem(), QStringLiteral("uiTasksTitle")) != nullptr);
        app_->navigate(QStringLiteral("profile"));

        // stats 任务只会被 navigate 漏斗点亮（本二进制无其它用例进月报页）。
        auto taskDone = [prog](const QString& id) {
            const QVariantList rows = prog->tasks();
            for (const QVariant& row : rows)
                if (row.toMap().value(QStringLiteral("id")).toString() == id)
                    return row.toMap().value(QStringLiteral("done")).toBool();
            return true;
        };
        QVERIFY(!taskDone(QStringLiteral("stats")));
        const qint64 before = prog->xp();
        app_->navigate(QStringLiteral("stats"));
        QCOMPARE(prog->xp(), before + 20);
        QVERIFY(taskDone(QStringLiteral("stats")));
        app_->navigate(QStringLiteral("profile"));    // 同事件再进：幂等不加经验
        app_->navigate(QStringLiteral("stats"));
        QCOMPARE(prog->xp(), before + 20);
    }
};

QTEST_MAIN(QmlStationInteractionsTest)
#include "tst_qml_station_interactions.moc"
