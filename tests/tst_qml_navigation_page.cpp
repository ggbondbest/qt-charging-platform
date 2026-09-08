#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <memory>

// No key or external request is needed to exercise the page state machine.
class NavigationMapFake final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasLocation MEMBER hasLocation NOTIFY locationChanged)
    Q_PROPERTY(QString locationLabel MEMBER locationLabel NOTIFY locationChanged)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
    Q_PROPERTY(QString routeHtml MEMBER routeHtml NOTIFY changed)
    Q_PROPERTY(int routeDistanceMeters MEMBER routeDistanceMeters NOTIFY changed)
    Q_PROPERTY(int durationMinutes MEMBER durationMinutes NOTIFY changed)
    Q_PROPERTY(QVariantList steps MEMBER steps NOTIFY changed)
public:
    bool hasLocation = false;
    QString locationLabel;
    bool busy = false;
    QString error, routeHtml;
    int routeDistanceMeters = -1, durationMinutes = -1;
    QVariantList steps;
    QStringList addresses;
    QStringList modes;
    QList<double> targetLatitudes;
    int cancelCalls = 0;

    Q_INVOKABLE void geocodeAddress(const QString& address)
    {
        addresses << address;
        busy = true;
        error.clear();
        emit changed();
    }
    Q_INVOKABLE void requestRoute(double latitude, double, const QString& mode)
    {
        modes << mode;
        targetLatitudes << latitude;
        busy = true;
        emit changed();
    }
    Q_INVOKABLE void cancelRoute() { ++cancelCalls; }
    void finishGeocode(bool duplicateSignal = false)
    {
        hasLocation = true;
        busy = false;
        locationLabel = addresses.last();
        emit locationChanged();
        if (duplicateSignal) emit locationChanged();
        emit changed();
    }
    void finishRoute()
    {
        busy = false;
        routeDistanceMeters = 4400;
        durationMinutes = 12;
        steps = {QVariantMap{{QStringLiteral("instruction"), QStringLiteral("沿软件园路行驶，前方右转进入数码路")}},
                 QVariantMap{{QStringLiteral("instruction"), QStringLiteral("沿数码路行驶，到达充电站入口")}}};
        emit changed();
    }
signals:
    void changed();
    void locationChanged();
};

class NavigationAppFake final : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void back() { ++backCalls; }
    int backCalls = 0;
};

class QmlNavigationPageTest final : public QObject
{
    Q_OBJECT
    std::unique_ptr<QQuickItem> load(QQmlEngine& engine, QQuickWindow& window,
                                    NavigationMapFake& map, NavigationAppFake& app)
    {
        engine.rootContext()->setContextProperty(QStringLiteral("mapBridge"), &map);
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
            + QStringLiteral("/pages/station/NavigationPage.qml")));
        if (component.isError()) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        const QVariantMap station{{QStringLiteral("stationName"), QStringLiteral("海创中心示范充电站")},
                                  {QStringLiteral("stationLatitude"), 38.876},
                                  {QStringLiteral("stationLongitude"), 121.53}};
        QObject* object = component.createWithInitialProperties({{QStringLiteral("arg"), station}});
        auto* page = qobject_cast<QQuickItem*>(object);
        if (!page) {
            delete object;
            return nullptr;
        }
        page->setParentItem(window.contentItem());
        return std::unique_ptr<QQuickItem>(page);
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle(QStringLiteral("Basic")); }

    void missingOriginDoesNotRequestAFakeRoute()
    {
        NavigationMapFake map;
        NavigationAppFake app;
        QQmlEngine engine;
        QQuickWindow window;
        auto page = load(engine, window, map, app);
        QVERIFY(page);
        QCoreApplication::processEvents();
        QVERIFY(map.modes.isEmpty());
        auto* caption = page->findChild<QObject*>(QStringLiteral("navigationCaptionLabel"));
        QVERIFY(caption);
        QVERIFY(caption->property("text").toString().contains(QStringLiteral("请先定位")));
        QVERIFY(page->property("loadedHtml").toString().isEmpty());
    }

    void invalidDestinationDoesNotRequestRoute_data()
    {
        QTest::addColumn<QVariantMap>("destination");
        QTest::newRow("explicitly-missing-zero-coordinates") << QVariantMap{
            {QStringLiteral("stationName"), QStringLiteral("无坐标预约")},
            {QStringLiteral("hasStationLocation"), false},
            {QStringLiteral("stationLatitude"), 0.0},
            {QStringLiteral("stationLongitude"), 0.0}};
        QTest::newRow("out-of-range") << QVariantMap{
            {QStringLiteral("stationLatitude"), 91.0},
            {QStringLiteral("stationLongitude"), 121.53}};
        QTest::newRow("missing-longitude") << QVariantMap{
            {QStringLiteral("stationLatitude"), 38.88}};
    }

    void invalidDestinationDoesNotRequestRoute()
    {
        QFETCH(QVariantMap, destination);
        NavigationMapFake map;
        NavigationAppFake app;
        QQmlEngine engine;
        QQuickWindow window;
        auto page = load(engine, window, map, app);
        QVERIFY(page);
        QVERIFY(page->setProperty("arg", destination));
        map.hasLocation = true;
        QVERIFY(QMetaObject::invokeMethod(page.get(), "requestRoute"));
        QVERIFY(page->property("localError").toString().contains(QStringLiteral("缺少有效坐标")));
        QVERIFY(map.modes.isEmpty());
        QVERIFY(!page->property("ownsRoute").toBool());
        QVERIFY(page->property("loadedHtml").toString().isEmpty());
        QCoreApplication::processEvents();
        QVERIFY(map.modes.isEmpty()); // an old zero-delay timer must not retry the invalid target

        // Old station arguments omit the flag and use plain latitude/longitude.
        // They remain supported and can recover immediately after rejection.
        QVERIFY(page->setProperty("arg", QVariantMap{
            {QStringLiteral("stationName"), QStringLiteral("兼容站点参数")},
            {QStringLiteral("latitude"), 38.88},
            {QStringLiteral("longitude"), 121.53}}));
        QTRY_COMPARE(map.modes.size(), 1);
        QCOMPARE(map.targetLatitudes.last(), 38.88);
        QVERIFY(page->property("localError").toString().isEmpty());
    }

    void missingDestinationReleasesPreviouslyOwnedRoute()
    {
        NavigationMapFake map;
        map.hasLocation = true;
        NavigationAppFake app;
        QQmlEngine engine;
        QQuickWindow window;
        auto page = load(engine, window, map, app);
        QVERIFY(page);
        QTRY_COMPARE(map.modes.size(), 1);
        map.finishRoute();
        QVERIFY(page->property("ownsRoute").toBool());
        QVERIFY(page->setProperty("arg", QVariantMap{
            {QStringLiteral("hasStationLocation"), false},
            {QStringLiteral("stationLatitude"), 0.0},
            {QStringLiteral("stationLongitude"), 0.0}}));
        QTRY_COMPARE(map.cancelCalls, 1);
        QCOMPARE(map.modes.size(), 1);
        QVERIFY(!page->property("ownsRoute").toBool());
        QVERIFY(page->property("loadedHtml").toString().isEmpty());
        QVERIFY(page->property("localError").toString().contains(QStringLiteral("缺少有效坐标")));
    }

    void duplicatePositionSignalsPlanOnlyOnceAndCanLocateAgain()
    {
        NavigationMapFake map;
        NavigationAppFake app;
        QQmlEngine engine;
        QQuickWindow window;
        auto page = load(engine, window, map, app);
        QVERIFY(page);
        auto* input = page->findChild<QObject*>(QStringLiteral("originField"));
        QVERIFY(input);
        input->setProperty("text", QStringLiteral("高新区软件园路"));
        QVERIFY(QMetaObject::invokeMethod(page.get(), "locateOrigin"));
        QVERIFY(QMetaObject::invokeMethod(page.get(), "locateOrigin"));
        QCOMPARE(map.addresses.size(), 1);
        QCOMPARE(map.addresses.first(), QStringLiteral("大连市 高新区软件园路"));
        QVERIFY(page->property("locating").toBool());
        map.finishGeocode(true);
        QTRY_COMPARE(map.modes.size(), 1);
        QCOMPARE(map.modes.first(), QStringLiteral("driving"));
        QVERIFY(!page->property("locating").toBool());
        map.finishRoute();
        input->setProperty("text", QStringLiteral("高新区黄浦路"));
        QVERIFY(QMetaObject::invokeMethod(page.get(), "locateOrigin"));
        QCOMPARE(map.addresses.size(), 2);
        map.finishGeocode(true);
        QTRY_COMPARE(map.modes.size(), 2);
    }

    void geocodeFailureReleasesSubmitGate()
    {
        NavigationMapFake map;
        NavigationAppFake app;
        QQmlEngine engine;
        QQuickWindow window;
        auto page = load(engine, window, map, app);
        QVERIFY(page);
        page->findChild<QObject*>(QStringLiteral("originField"))->setProperty("text", QStringLiteral("测试路"));
        QVERIFY(QMetaObject::invokeMethod(page.get(), "locateOrigin"));
        map.busy = false;
        map.error = QStringLiteral("地址解析失败 [status 123]");
        emit map.changed();
        QVERIFY(!page->property("locating").toBool());
        auto* caption = page->findChild<QObject*>(QStringLiteral("navigationCaptionLabel"));
        QVERIFY(caption->property("text").toString().contains(QStringLiteral("status 123")));
        QVERIFY(QMetaObject::invokeMethod(page.get(), "locateOrigin"));
        QCOMPARE(map.addresses.size(), 2);
    }

    void existingOriginAndModeSwitchUseMapBridge()
    {
        NavigationMapFake map;
        map.hasLocation = true;
        map.locationLabel = QStringLiteral("北京市 天安门广场");
        NavigationAppFake app;
        QQmlEngine engine;
        QQuickWindow window;
        auto page = load(engine, window, map, app);
        QVERIFY(page);
        QTRY_COMPARE(map.modes.size(), 1);
        QCOMPARE(page->findChild<QObject*>(QStringLiteral("originField"))->property("text").toString(),
                 QStringLiteral("天安门广场"));
        QCOMPARE(page->findChild<QObject*>(QStringLiteral("navigationRegionInput"))->property("currentIndex").toInt(), 2);
        map.finishRoute();
        auto* walking = page->findChild<QObject*>(QStringLiteral("walkingRouteButton"));
        QVERIFY(walking);
        QVERIFY(QMetaObject::invokeMethod(walking, "clicked"));
        QCOMPARE(map.modes.size(), 2);
        QCOMPARE(map.modes.last(), QStringLiteral("walking"));
        page.reset();
        QCOMPARE(map.cancelCalls, 1);
    }

    void layoutDoesNotOverlap_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("standard") << QSize(420, 740);
        QTest::newRow("compact") << QSize(360, 520);
    }

    void hiddenStackPagesCannotRequestOrCancelSuccessorsRoute()
    {
        NavigationMapFake map;
        map.hasLocation = true;
        map.locationLabel = QStringLiteral("大连市 软件园路");
        NavigationAppFake app;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("mapBridge"), &map);
        engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);
        QQuickWindow window;
        window.resize(420, 740);
        QQmlComponent component(&engine);
        const QByteArray navigationUrl = QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
            + QStringLiteral("/pages/station/NavigationPage.qml")).toEncoded();
        component.setData(QByteArrayLiteral(
            "import QtQuick\nimport QtQuick.Controls.Basic\n"
            "Item { width: 420; height: 740; property alias stack: stack; "
            "function pushDestination(latitude, label) { stack.push(\"") + navigationUrl
            + QByteArrayLiteral("\", {arg: {stationName: label, stationLatitude: latitude, "
                                "stationLongitude: 121.53}}, StackView.Immediate); } "
                                "function popDestination() { stack.pop(StackView.Immediate); } "
                                "StackView { id: stack; anchors.fill: parent } }"),
            QUrl(QStringLiteral("file:///navigation-stack-test.qml")));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> host(component.create());
        QVERIFY(host);
        qobject_cast<QQuickItem*>(host.get())->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto execute = [&engine, &host](const QString& code) {
            QQmlExpression expression(engine.rootContext(), host.get(), code);
            expression.evaluate();
            return !expression.hasError();
        };
        QVERIFY(execute(QStringLiteral("pushDestination(38.87, '第一个电站')")));
        QTRY_COMPARE(map.modes.size(), 1);
        QCOMPARE(map.targetLatitudes.last(), 38.87);
        auto* stack = host->property("stack").value<QObject*>();
        QVERIFY(stack);
        QObject* firstPage = stack->property("currentItem").value<QObject*>();
        QVERIFY(firstPage);
        map.finishRoute();

        QVERIFY(execute(QStringLiteral("pushDestination(38.96, '第二个电站')")));
        QTRY_COMPARE(map.modes.size(), 2);
        QCOMPARE(map.targetLatitudes.last(), 38.96);
        QCOMPARE(map.cancelCalls, 1);
        QVERIFY(!firstPage->property("isActivePage").toBool());
        QVERIFY(!firstPage->property("ownsRoute").toBool());
        map.finishRoute();
        emit map.locationChanged();
        QTRY_COMPARE(map.modes.size(), 3);
        QCOMPARE(map.targetLatitudes.last(), 38.96);
        map.finishRoute();

        QVERIFY(execute(QStringLiteral("popDestination()")));
        QTRY_COMPARE(map.modes.size(), 4);
        QCOMPARE(map.targetLatitudes.last(), 38.87);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        // The popped page already relinquished ownership while deactivating.
        // Its eventual destructor must not cancel the resumed page's request.
        QCOMPARE(map.cancelCalls, 2);
        QVERIFY(firstPage->property("ownsRoute").toBool());
    }

    void layoutDoesNotOverlap()
    {
        QFETCH(QSize, size);
        NavigationMapFake map;
        NavigationAppFake app;
        QQmlEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&warnings](const QList<QQmlError>& list) {
            for (const auto& item : list) warnings << item.toString();
        });
        QQuickWindow window;
        window.resize(size);
        auto page = load(engine, window, map, app);
        QVERIFY(page);
        map.finishRoute();
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(50);
        auto* mapCard = page->findChild<QQuickItem*>(QStringLiteral("navigationMapCard"));
        auto* steps = page->findChild<QQuickItem*>(QStringLiteral("navigationStepsList"));
        auto* back = page->findChild<QQuickItem*>(QStringLiteral("navigationBackButton"));
        auto* caption = page->findChild<QQuickItem*>(QStringLiteral("navigationCaptionLabel"));
        QVERIFY(mapCard && steps && back && caption);
        QVERIFY(mapCard->height() >= 200);
        QVERIFY(steps->mapToItem(page.get(), QPointF(0, 0)).y()
                >= mapCard->mapToItem(page.get(), QPointF(0, mapCard->height())).y());
        QVERIFY(mapCard->mapToItem(page.get(), QPointF(0, 0)).y()
                >= caption->mapToItem(page.get(), QPointF(0, caption->height())).y());
        QVERIFY(back->mapToItem(page.get(), QPointF(0, 0)).y()
                >= steps->mapToItem(page.get(), QPointF(0, steps->height())).y());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));

        const QString screenshotDir = qEnvironmentVariable("CHARGING_UI_SCREENSHOT_DIR");
        if (!screenshotDir.isEmpty()) {
            auto shot = page->grabToImage();
            QTRY_VERIFY(!shot->image().isNull());
            QVERIFY(shot->saveToFile(screenshotDir + QStringLiteral("/navigation-%1.png").arg(size.width())));
        }
    }
};

QTEST_MAIN(QmlNavigationPageTest)
#include "tst_qml_navigation_page.moc"
