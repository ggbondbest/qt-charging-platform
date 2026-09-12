#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <memory>

namespace {
QVariantMap order(const QString& id = QStringLiteral("17"), const QString& state = QStringLiteral("charging"))
{
    return {{"id", id}, {"status", state}, {"stationName", QStringLiteral("高新园区示范充电站")},
            {"chargerCode", "CHG-DEMO-001-A1"}, {"durationSeconds", 53},
            {"energyWh", 1270}, {"amountCents", 152}, {"powerWatts", 120000}, {"powerKnown", true}};
}

class AppFake final : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void navigate(const QString& route, const QVariant& arg = {})
    { emit navigateRequested(route, arg); }
    Q_INVOKABLE void showToast(const QString&, const QString&) {}
signals:
    void navigateRequested(const QString& route, const QVariant& arg);
};
class OrderFake final : public QObject
{
    Q_OBJECT
public:
    bool fetching = false;
    int requests = 0;
    Q_INVOKABLE void fetchStatusCounts() {}
    Q_INVOKABLE bool isFetchingOrders() const { return fetching; }
    Q_INVOKABLE void fetchOrders(const QString&, int) { fetching = true; ++requests; }
    void respond(const QVariantList& rows) {
        fetching = false; emit ordersLoaded(rows, rows.size(), false);
    }
signals:
    void statusCountsUpdated(int chargingCount, int waitingPaymentCount, int completedCount);
    void ordersLoaded(const QVariantList& rows, int total, bool hasMore);
    void operationFailed(const QString& type, const QString& code, const QString& message);
};
class ChargingFake final : public QObject
{
    Q_OBJECT
public:
    QStringList tracking;
    int stops = 0;
    int releases = 0;
    int statusFetches = 0;
    QString startedReservation;
    QString targetType;
    double targetValue = 0;
    Q_INVOKABLE void startTracking(const QString& id) { tracking << id; }
    Q_INVOKABLE void stopTracking() { ++releases; }
    Q_INVOKABLE void stopCharging() { ++stops; }
    Q_INVOKABLE bool isStarting() const { return false; }
    Q_INVOKABLE void startCharging(const QString&) {}
    Q_INVOKABLE void fetchStatusNow() { ++statusFetches; }
    Q_INVOKABLE void startChargingWithTarget(const QString& id, const QString& type, double value)
    { startedReservation = id; targetType = type; targetValue = value; }
signals:
    void startCompleted(const QVariantMap& status);
    void statusLoaded(const QVariantMap& status);
    void stopCompleted(const QVariantMap& status);
    void operationFailed(const QString& type, const QString& code, const QString& message);
};
class ReservationFake final : public QObject
{
    Q_OBJECT
public:
    QStringList cancelled;
    Q_INVOKABLE void fetchList() {}
    Q_INVOKABLE void cancel(const QString& id) { cancelled << id; }
signals:
    void listSucceeded(const QVariantList& records);
    void listFailed(const QString& message);
    void cancelSucceeded(const QVariant& id);
    void cancelFailed(const QString& message);
};
struct Fixture {
    AppFake app;
    OrderFake orders;
    ChargingFake charging;
    ReservationFake reservation;
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QObject> appearance;
    std::unique_ptr<QQuickItem> page;

    explicit Fixture(const QString& file = QStringLiteral("ChargingHomePage.qml"), QSize size = {420, 720})
    {
        engine.rootContext()->setContextProperty("App", &app);
        engine.rootContext()->setContextProperty("orderService", &orders);
        engine.rootContext()->setContextProperty("chargingService", &charging);
        engine.rootContext()->setContextProperty("reservationService", &reservation);
        QQmlComponent config(&engine);
        const auto style = QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR) + "/platform").toEncoded();
        config.setData("import QtQuick\nimport \"" + style +
            "\" as P\nQtObject { property string theme: \"light\"; property string scaleName: \"standard\";"
            " onThemeChanged: P.Style.theme = theme; onScaleNameChanged: P.Style.fontScale = scaleName;"
            " Component.onCompleted: P.Style.motionEnabled = false }", QUrl("file:///charging-test-style.qml"));
        appearance.reset(config.create());
        QQmlComponent c(&engine, QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
                                                   + "/pages/profile_charging/" + file));
        page.reset(qobject_cast<QQuickItem*>(c.create()));
        if (!page) { qWarning().noquote() << c.errorString(); return; }
        window.resize(size);
        page->setParentItem(window.contentItem());
        window.show();
    }
    void active() {
        orders.respond({order()});
        emit charging.statusLoaded(order());
    }
};
} // namespace

class QmlChargingUnifiedTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QQuickStyle::setStyle("Basic"); }

    void homeOwnsOneTrackerAndRejectsOtherOrders()
    {
        Fixture f;
        QVERIFY(f.page);
        f.active();
        QCOMPARE(f.charging.tracking, QStringList{"17"});
        QCOMPARE(f.page->property("kw").toDouble(), 120.0);
        emit f.charging.statusLoaded(order("999"));
        QCOMPARE(f.page->property("status").toMap().value("id").toString(), QString("17"));
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "refreshAll"));
        f.orders.respond({order()});
        QCOMPARE(f.charging.tracking.size(), 1); // Refresh does not restart the same tracker.
        QTest::qWait(1100);
        QCOMPARE(f.page->property("seconds").toInt(), 53); // Never invent time during a lost connection.
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "releaseTracking"));
        QCOMPARE(f.charging.releases, 1);
        f.page.reset();
        QCOMPARE(f.charging.releases, 1); // Shell release + deferred destruction is idempotent.
    }

    void stopPreventsDuplicatesCanRetryAndUsesAuthoritativeSettlement()
    {
        Fixture f;
        QVERIFY(f.page);
        f.active();
        QSignalSpy routes(&f.app, &AppFake::navigateRequested);
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "stopCurrentCharging"));
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "stopCurrentCharging"));
        QCOMPARE(f.charging.stops, 1);
        QVERIFY(f.page->property("stopping").toBool());
        emit f.charging.operationFailed("STOP_CHARGING", "NETWORK", QStringLiteral("连接超时"));
        QVERIFY(!f.page->property("stopping").toBool());
        QVERIFY(f.page->property("statusError").toString().contains(QStringLiteral("停止失败")));
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "stopCurrentCharging"));
        QCOMPARE(f.charging.stops, 2);
        auto settled = order("17", "waiting_payment");
        settled.insert("amountCents", 199);
        emit f.charging.stopCompleted(settled);
        QCOMPARE(routes.size(), 1);
        QCOMPARE(routes.at(0).at(0).toString(), QString("settlement"));
        QCOMPARE(routes.at(0).at(1).toMap().value("amountCents").toInt(), 199);
        emit f.charging.statusLoaded(settled); // Late polling cannot navigate twice.
        QCOMPARE(routes.size(), 1);
        QCOMPARE(f.charging.releases, 1);
    }

    void recoveredTerminalStatusRoutesToPayment()
    {
        Fixture f("ChargingPage.qml");
        QVERIFY(f.page);
        QCOMPARE(f.page->objectName(), QString("chargingHomePage")); // No second visual implementation.
        f.active();
        QSignalSpy routes(&f.app, &AppFake::navigateRequested);
        emit f.charging.statusLoaded(order("17", "waiting_payment"));
        QCOMPARE(routes.size(), 1);
        QCOMPARE(routes.at(0).at(0).toString(), QString("settlement"));
        QVERIFY(!f.page->findChild<QObject*>("viewChargingRunButton"));
    }

    void targetDialogConfirmsIntegerBudgetBeforeStart()
    {
        Fixture f;
        QVERIFY(f.page);
        QSignalSpy warnings(&f.engine, &QQmlEngine::warnings);
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "startReservation", Q_ARG(QVariant, "123")));
        QVERIFY(!f.page->property("startPending").toBool());
        auto* dialog = f.page->findChild<QObject*>("chargingTargetDialog");
        QVERIFY(dialog);
        QVERIFY(dialog->property("visible").toBool());
        QCOMPARE(dialog->property("normalized").toDouble(), 2000.0);
        auto* confirm = dialog->findChild<QObject*>("confirmChargingTargetButton");
        QVERIFY(confirm);
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QCOMPARE(f.charging.startedReservation, QStringLiteral("123"));
        QCOMPARE(f.charging.targetType, QStringLiteral("AMOUNT"));
        QCOMPARE(f.charging.targetValue, 2000.0);
        QVERIFY(f.page->property("startPending").toBool());
        QVERIFY2(warnings.isEmpty(), "Target dialog emitted a binding warning");
    }

    void autoStopListRefreshDoesNotLoseSettlement()
    {
        Fixture f;
        QVERIFY(f.page);
        f.active();
        auto live = order();
        live.insert("target", QVariantMap{{"type", "AMOUNT"}, {"value", 2000},
                    {"completedValue", 152}, {"remainingValue", 1848}, {"progressPercent", 7.6}});
        emit f.charging.statusLoaded(live);
        auto* progress = f.page->findChild<QQuickItem*>("chargingTargetProgress");
        QVERIFY(progress && progress->isVisible());
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "refreshAll"));
        f.orders.respond({});
        QCOMPARE(f.charging.statusFetches, 1);
        QCOMPARE(f.charging.releases, 0);
        QSignalSpy routes(&f.app, &AppFake::navigateRequested);
        auto finished = order("17", "waiting_payment");
        finished.insert("stopReason", "TARGET_AMOUNT");
        emit f.charging.statusLoaded(finished);
        QCOMPARE(routes.size(), 1);
        QCOMPARE(routes.at(0).at(0).toString(), QStringLiteral("settlement"));
        QCOMPARE(f.charging.releases, 1);
    }

    void targetDialogFitsSmallLargeFontWindow()
    {
        Fixture f("ChargingHomePage.qml", {320, 540});
        QVERIFY(f.page);
        f.appearance->setProperty("scaleName", "extraLarge");
        QSignalSpy warnings(&f.engine, &QQmlEngine::warnings);
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "startReservation", Q_ARG(QVariant, "123")));
        QTest::qWait(100);
        auto* dialog = f.page->findChild<QObject*>("chargingTargetDialog");
        QVERIFY(dialog && dialog->property("visible").toBool());
        QVERIFY(dialog->property("height").toReal() <= 516);
        QVERIFY2(warnings.isEmpty(), "Small target dialog emitted a binding warning");
        const QString out = qEnvironmentVariable("CHARGING_UI_CAPTURE_DIR");
        if (!out.isEmpty()) {
            QDir().mkpath(out);
            QVERIFY(f.window.grabWindow().save(out + "/charging-target-dialog-small.png"));
        }
    }

    void cancellationIsConfirmedAndPendingBlocksStart()
    {
        Fixture f;
        QVERIFY(f.page);
        const QVariantMap reservation{{"id", "123"}, {"status", "active"},
            {"stationName", QStringLiteral("测试站")}, {"chargerCode", "A1"},
            {"expiresAtUtc", QDateTime::currentDateTimeUtc().addSecs(600).toString(Qt::ISODate)}};
        emit f.reservation.listSucceeded({reservation});
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "requestCancel", Q_ARG(QVariant, reservation)));
        QVERIFY(f.reservation.cancelled.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "confirmCancel"));
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "confirmCancel"));
        QCOMPARE(f.reservation.cancelled, QStringList{"123"});
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "startReservation", Q_ARG(QVariant, "123")));
        QVERIFY(!f.page->property("startPending").toBool());
        emit f.reservation.cancelFailed(QStringLiteral("网络异常"));
        QCOMPARE(f.page->property("pendingCancelId").toString(), QString());
        QVERIFY(!f.page->property("loadError").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(f.page.get(), "confirmCancel"));
        emit f.reservation.cancelSucceeded(QString("123"));
        QVERIFY(f.page->property("pendingCancelId").toString().isEmpty());
    }

    void presentation_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("theme");
        QTest::addColumn<QString>("scale");
        QTest::newRow("regular") << QSize(420, 720) << QStringLiteral("light") << QStringLiteral("standard");
        QTest::newRow("small-large-dark") << QSize(320, 540) << QStringLiteral("dark") << QStringLiteral("extraLarge");
        QTest::newRow("wide") << QSize(720, 840) << QStringLiteral("light") << QStringLiteral("large");
    }
    void presentation()
    {
        QFETCH(QSize, size); QFETCH(QString, theme); QFETCH(QString, scale);
        Fixture f("ChargingHomePage.qml", size);
        QVERIFY(f.page);
        QSignalSpy warnings(&f.engine, &QQmlEngine::warnings);
        f.appearance->setProperty("theme", theme);
        f.appearance->setProperty("scaleName", scale);
        f.active();
        auto live = order();
        live.insert("target", QVariantMap{{"type", "AMOUNT"}, {"value", 2000},
                    {"completedValue", 152}, {"remainingValue", 1848}, {"progressPercent", 7.6}});
        emit f.charging.statusLoaded(live);
        QVERIFY(QTest::qWaitForWindowExposed(&f.window));
        QTest::qWait(100);
        auto* hero = f.page->findChild<QQuickItem*>("uiChargingHero");
        auto* stats = f.page->findChild<QQuickItem*>("chargingStats");
        auto* stop = f.page->findChild<QQuickItem*>("stopChargingBar");
        auto* power = f.page->findChild<QQuickItem*>("chargingPowerValue");
        QVERIFY(hero && stats && stop && power);
        QVERIFY(hero->height() > 200);
        const QRectF statsRect = stats->mapRectToItem(hero, stats->boundingRect());
        QVERIFY(statsRect.bottom() <= hero->height());
        QVERIFY(statsRect.left() >= 0);
        QVERIFY(statsRect.right() <= hero->width() + 1);
        QVERIFY(stop->mapToItem(hero, QPointF(0, 0)).y() >= hero->height());
        QVERIFY(hero->mapToItem(f.page.get(), QPointF(hero->width(), 0)).x() <= size.width());
        QCOMPARE(power->property("color").value<QColor>(), QColor(Qt::white));
        QVERIFY2(warnings.isEmpty(), "QML emitted runtime/binding warnings");
        const QString out = qEnvironmentVariable("CHARGING_UI_CAPTURE_DIR");
        if (!out.isEmpty()) {
            QDir().mkpath(out);
            QVERIFY(f.window.grabWindow().save(out + "/charging-" + QString::fromLatin1(QTest::currentDataTag()) + ".png"));
        }
        // The extra-large small-window layout scrolls instead of hiding the
        // destructive action below a fixed-height, non-scrollable container.
        for (auto* child : f.page->findChildren<QQuickItem*>()) {
            if (!child->inherits("QQuickFlickable")) continue;
            const qreal bottom = child->property("contentHeight").toReal() - child->height();
            child->setProperty("contentY", qMax<qreal>(0, bottom));
        }
        QTest::qWait(30);
        const auto stopCenter = stop->mapToScene(QPointF(stop->width() / 2, stop->height() / 2));
        QVERIFY(stopCenter.y() > 0 && stopCenter.y() < size.height());
        QTest::mouseClick(&f.window, Qt::LeftButton, Qt::NoModifier, stopCenter.toPoint());
        QCOMPARE(f.charging.stops, 1);
    }
};

QTEST_MAIN(QmlChargingUnifiedTest)
#include "tst_qml_charging_unified.moc"
