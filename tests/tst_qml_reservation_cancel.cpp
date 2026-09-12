#include "app_bridge.h"
#include "service_bridges.h"
#include "server_runtime.h"
#include "services/reservation/reservation_service.h"

#include <QQmlComponent>
#include <QDir>
#include <QImage>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace charging::qml;
using charging::server::ServerRuntime;

class QmlReservationCancelTest final : public QObject
{
    Q_OBJECT
    QTemporaryDir settings_;

    static QString storedState(const QString& path, const QString& table, const QString& id)
    {
        const QString name = QUuid::createUuid().toString();
        QString state;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(path);
            database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
            if (database.open()) {
                QSqlQuery query(database);
                query.prepare(QStringLiteral("SELECT status FROM %1 WHERE id = ?").arg(table));
                query.addBindValue(id);
                if (query.exec() && query.next()) state = query.value(0).toString();
                else qWarning() << query.lastError();
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
        return state;
    }

    static void bind(QQmlEngine& engine, QmlApp& app)
    {
        auto* context = engine.rootContext();
        context->setContextProperty("App", &app);
        context->setContextProperty("reservationService", app.reservationService());
        context->setContextProperty("chargingService", app.chargingService());
        context->setContextProperty("orderService", app.orderService());
        context->setContextProperty("ratingsService", app.ratingsService());
    }

    static QQuickItem* load(QQmlEngine& engine, const QString& filename,
                            QQuickWindow& window, const QVariantMap& properties = {})
    {
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QStringLiteral(CHARGING_QML_SOURCE_DIR) + "/pages/" + filename));
        if (component.isError()) qWarning().noquote() << component.errorString();
        auto* item = qobject_cast<QQuickItem*>(component.createWithInitialProperties(properties));
        if (item) { item->setParent(&window); item->setParentItem(window.contentItem()); }
        return item;
    }

    static QQuickItem* visualChild(QQuickItem* item, const QString& name)
    {
        if (item->objectName() == name) return item;
        for (auto* child : item->childItems())
            if (auto* found = visualChild(child, name)) return found;
        return nullptr;
    }

    // Exercise pointer delivery, including modal popup hit-testing, rather than
    // invoking service methods and calling that a UI regression test.
    static bool ready(QQuickWindow& window, const QString& name)
    {
        auto* button = visualChild(window.contentItem(), name);
        if (!button || !button->isVisible() || !button->isEnabled()
            || button->width() <= 0 || button->height() <= 0) return false;
        const QPoint point = button->mapToScene(QPointF(button->width() / 2,
                                                       button->height() / 2)).toPoint();
        return QRect(QPoint(), window.size()).contains(point);
    }

    static bool click(QQuickWindow& window, const QString& name)
    {
        // Finish the current frame's layout / popup transition before deriving
        // screen coordinates. Visibility alone does not mean polish has run.
        QTest::qWait(120);
        if (!ready(window, name)) return false;
        auto* button = visualChild(window.contentItem(), name);
        QSignalSpy clicked(button, SIGNAL(clicked()));
        const QPoint point = button->mapToScene(QPointF(button->width() / 2,
                                                       button->height() / 2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
        return clicked.size() == 1;
    }

    static bool capture(QQuickWindow& window, const QString& state)
    {
        const auto directory = qEnvironmentVariable("CHARGING_RESERVATION_CAPTURE_DIR");
        const auto tag = QString::fromLatin1(QTest::currentDataTag());
        if (directory.isEmpty() || !tag.startsWith("reservation-module")) return true;
        if (!QDir().mkpath(directory)) return false;
        QTest::qWait(150);
        return window.grabWindow().save(QDir(directory).filePath(tag + "-" + state + ".png"));
    }

    static QVariantMap reserve(QmlApp& app)
    {
        auto* stations = qobject_cast<StationQueryBridge*>(app.stationQueryService());
        QSignalSpy listed(stations, &StationQueryBridge::querySucceeded);
        stations->search();
        if (listed.isEmpty() && !listed.wait(8000)) return {};
        const auto allStations = listed.first().first().toList();
        if (allStations.isEmpty()) return {};
        const auto station = allStations.first().toMap();
        QSignalSpy detailed(stations, &StationQueryBridge::detailSucceeded);
        stations->fetchDetailById(station.value("id"), -1);
        if (detailed.isEmpty() && !detailed.wait(8000)) return {};
        QVariantMap charger;
        for (const auto& item : detailed.first().first().toMap().value("chargers").toList()) {
            if (item.toMap().value("status") == "available") { charger = item.toMap(); break; }
        }
        if (charger.isEmpty()) return {};
        auto* service = qobject_cast<ReservationBridge*>(app.reservationService());
        QSignalSpy created(service, &ReservationBridge::submitSucceeded);
        service->submit({{"stationId", station.value("id")}, {"chargerId", charger.value("id")},
                         {"stationName", station.value("name")}, {"chargerCode", charger.value("code")},
                         {"chargerPowerWatts", charger.value("powerWatts")}});
        if (created.isEmpty() && !created.wait(8000)) return {};
        return created.first().first().toMap();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatformTests"));
        QCoreApplication::setApplicationName(QStringLiteral("qml-reservation-cancel"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.path());
        qunsetenv("TENCENT_MAP_API_KEY");
        qunsetenv("CHARGING_TENCENT_MAP_KEY");
        qunsetenv("TENCENT_MAP_JS_KEY");
    }

    void pointerCancelPersistsAndReleasesCharger_data()
    {
        QTest::addColumn<QString>("filename");
        QTest::addColumn<QString>("cancelButton");
        QTest::addColumn<QString>("keepButton");
        QTest::addColumn<QString>("confirmButton");
        QTest::addColumn<QSize>("size");
        QTest::newRow("reservation-module") << "station/ReservationModulePage.qml"
            << "reservationOrderCancelButton" << "reservationCancelKeepButton"
            << "reservationCancelConfirmButton" << QSize(420, 860);
        QTest::newRow("charging-home") << "profile_charging/ChargingHomePage.qml"
            << "chargingCancelReservationButton" << "keepChargingReservationButton"
            << "confirmChargingCancelButton" << QSize(420, 860);
        QTest::newRow("reservation-module-small") << "station/ReservationModulePage.qml"
            << "reservationOrderCancelButton" << "reservationCancelKeepButton"
            << "reservationCancelConfirmButton" << QSize(360, 640);
        QTest::newRow("charging-home-small") << "profile_charging/ChargingHomePage.qml"
            << "chargingCancelReservationButton" << "keepChargingReservationButton"
            << "confirmChargingCancelButton" << QSize(360, 640);
    }

    void pointerCancelPersistsAndReleasesCharger()
    {
        QFETCH(QString, filename);
        QFETCH(QString, cancelButton);
        QFETCH(QString, keepButton);
        QFETCH(QString, confirmButton);
        QFETCH(QSize, size);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto database = temp.filePath("cancel.sqlite");
        ServerRuntime runtime;
        QVERIFY(runtime.start(database, true, QHostAddress::LocalHost, 0));
        QTRY_VERIFY(runtime.isListening());
        QmlApp app("127.0.0.1", runtime.serverPort(), false);
        QVERIFY(app.login("13900139201"));
        QTRY_VERIFY(app.loggedIn());
        QTRY_VERIFY(!app.checkingOrders());
        const auto record = reserve(app);
        QVERIFY(!record.isEmpty());
        const QString id = record.value("id").toString();
        const QString orderId = record.value("orderId").toString();
        const QString chargerId = record.value("chargerId").toString();
        QCOMPARE(storedState(database, "reservations", id), QString("ACTIVE"));
        QCOMPARE(storedState(database, "chargers", chargerId), QString("RESERVED"));

        QQmlEngine engine;
        bind(engine, app);
        QQuickWindow window;
        window.resize(size); window.show();
        // Reserved orders also expose a discoverable entry, without treating
        // CANCEL_RESERVATION as a generic charging-order cancellation command.
        auto* detail = load(engine, "profile_charging/OrderDetailPage.qml", window,
            {{"arg", QVariantMap{{"id", orderId}, {"status", "reserved"},
                                  {"stationName", record.value("stationName")}}}});
        QVERIFY(detail);
        QSignalSpy navigated(&app, &QmlApp::navigateRequested);
        QTRY_VERIFY(ready(window, "orderManageReservationButton"));
        QVERIFY(click(window, "orderManageReservationButton"));
        QTRY_COMPARE(navigated.size(), 1);
        QCOMPARE(navigated.first().first().toString(), QString("reservation_module"));
        delete detail;
        auto* page = load(engine, filename, window);
        QVERIFY(page);
        auto* service = qobject_cast<ReservationBridge*>(app.reservationService());
        QSignalSpy cancelled(service, &ReservationBridge::cancelSucceeded);
        QSignalSpy failed(service, &ReservationBridge::cancelFailed);
        QTRY_VERIFY(visualChild(window.contentItem(), cancelButton));
        QTRY_VERIFY(ready(window, cancelButton));
        QVERIFY(capture(window, "active"));
        QVERIFY(click(window, cancelButton));
        QTRY_VERIFY(ready(window, keepButton));
        QVERIFY(click(window, keepButton));
        QCOMPARE(cancelled.size(), 0);
        QCOMPARE(storedState(database, "reservations", id), QString("ACTIVE"));
        QVERIFY(click(window, cancelButton));
        QTRY_VERIFY(ready(window, confirmButton));
        QVERIFY(capture(window, "confirm"));
        QVERIFY(click(window, confirmButton));
        QTRY_COMPARE(cancelled.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(storedState(database, "reservations", id), QString("CANCELLED"));
        QCOMPARE(storedState(database, "orders", orderId), QString("CANCELLED"));
        QCOMPARE(storedState(database, "chargers", chargerId), QString("AVAILABLE"));
        QTRY_VERIFY(!visualChild(window.contentItem(), cancelButton)
                    || !visualChild(window.contentItem(), cancelButton)->isVisible());
        delete page;
        // Cancellation retains history, and the user may reserve again.
        QVERIFY(!reserve(app).isEmpty());
    }

    void failureUnlocksAndCanRetryAgainstRealServer()
    {
        QTemporaryDir temp;
        const auto database = temp.filePath("retry.sqlite");
        ServerRuntime runtime;
        QVERIFY(runtime.start(database, true, QHostAddress::LocalHost, 0));
        QTRY_VERIFY(runtime.isListening());
        QmlApp app("127.0.0.1", runtime.serverPort(), false);
        QVERIFY(app.login("13900139202"));
        QTRY_VERIFY(app.loggedIn());
        QTRY_VERIFY(!app.checkingOrders());
        const auto valid = reserve(app);
        QVERIFY(!valid.isEmpty());
        QVariantMap stale = valid;
        stale["id"] = QStringLiteral("999999999");
        stale["reservationId"] = QStringLiteral("999999999");
        QQmlEngine engine;
        bind(engine, app);
        QQuickWindow window;
        window.resize(420, 860); window.show();
        auto* page = load(engine, "station/ReservationOrderPage.qml", window, {{"record", stale}});
        QVERIFY(page);
        auto* service = qobject_cast<ReservationBridge*>(app.reservationService());
        QSignalSpy failed(service, &ReservationBridge::cancelFailed);
        QSignalSpy started(service, &ReservationBridge::cancelStarted);
        QSignalSpy succeeded(service, &ReservationBridge::cancelSucceeded);
        QTRY_VERIFY(ready(window, "reservationOrderCancelButton"));
        QVERIFY(click(window, "reservationOrderCancelButton"));
        QTRY_VERIFY(ready(window, "reservationCancelConfirmButton"));
        QVERIFY(click(window, "reservationCancelConfirmButton"));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!page->property("cancelBusy").toBool());
        QVERIFY(!page->property("cancelNote").toString().isEmpty());
        auto* button = visualChild(window.contentItem(), "reservationOrderCancelButton");
        QVERIFY(button && button->isEnabled());

        QVERIFY(page->setProperty("record", valid));
        // The real request guard coalesces duplicate calls until its ACK.
        service->cancel(valid.value("id"));
        service->cancel(valid.value("id"));
        QCOMPARE(started.size(), 2); // one failed request + one retry, not three
        QTRY_COMPARE(succeeded.size(), 1);
        QCOMPARE(storedState(database, "chargers", valid.value("chargerId").toString()), QString("AVAILABLE"));
        delete page;

        const auto next = reserve(app);
        QVERIFY(!next.isEmpty());
        auto* charging = qobject_cast<ChargingBridge*>(app.chargingService());
        QSignalSpy chargingStarted(charging, &ChargingBridge::startCompleted);
        charging->startCharging(next.value("id"));
        QTRY_COMPARE(chargingStarted.size(), 1);
        service->cancel(next.value("id"));
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(storedState(database, "reservations", next.value("id").toString()), QString("FULFILLED"));
        QCOMPARE(storedState(database, "orders", next.value("orderId").toString()), QString("CHARGING"));
        QCOMPARE(storedState(database, "chargers", next.value("chargerId").toString()), QString("CHARGING"));
    }

    void disconnectedLiveServiceDoesNotUseMockCancellation()
    {
        charging::client::services::reservation::ReservationService service;
        service.setLiveMode(true);
        QSignalSpy failed(&service, &charging::client::services::reservation::ReservationService::cancelFailed);
        QSignalSpy succeeded(&service, &charging::client::services::reservation::ReservationService::cancelSucceeded);
        service.cancel(9001);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(succeeded.size(), 0);
        service.cancel(9001);
        QCOMPARE(failed.size(), 2); // pending ownership was cleared before failure
    }
};

QTEST_MAIN(QmlReservationCancelTest)
#include "tst_qml_reservation_cancel.moc"
