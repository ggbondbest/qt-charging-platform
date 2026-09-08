#include "app_bridge.h"
#include "service_bridges.h"
#include "admin_request_gateway.h"
#include "server_runtime.h"
#include "charging/common/model/models.h"
#include <QImage>
#include <QJsonArray>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QtTest>

using namespace charging::qml;
using namespace charging::server;

class QmlTcpDeliveryTest : public QObject
{
    Q_OBJECT
    QJsonObject admin(AdminRequestGateway& gateway, const QString& action,
                      const QJsonObject& data = {})
    {
        QSignalSpy replies(&gateway, &AdminRequestGateway::finished);
        const QString id = gateway.request(action, data, this);
        if (replies.isEmpty()) replies.wait(10000);
        for (const auto& reply : replies)
            if (reply.at(0).toString() == id) return reply.at(1).toJsonObject();
        return {};
    }
    void bind(QQmlEngine& engine, QmlApp& app)
    {
        auto* ctx = engine.rootContext();
        ctx->setContextProperty("App", &app);
        ctx->setContextProperty("walletService", app.walletService());
        ctx->setContextProperty("orderService", app.orderService());
        ctx->setContextProperty("chargingService", app.chargingService());
        ctx->setContextProperty("reservationService", app.reservationService());
    }
    QQuickItem* page(QQmlEngine& engine, const QString& name, QQuickWindow& window,
                     const QVariant& arg = QVariantMap{})
    {
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
            + "/pages/profile_charging/" + name));
        if (component.isError()) qWarning().noquote() << component.errorString();
        auto* item = qobject_cast<QQuickItem*>(component.createWithInitialProperties({{"arg", arg}}));
        if (item) { item->setParent(&window); item->setParentItem(window.contentItem()); }
        return item;
    }
private slots:
    void loginTimeoutNeverFallsBackToDemo()
    {
        QTcpServer silentServer;
        QVERIFY(silentServer.listen(QHostAddress::LocalHost, 0));
        QmlApp app("127.0.0.1", silentServer.serverPort(), false);
        QSignalSpy failed(&app, &QmlApp::loginFailed);
        QSignalSpy accepted(&app, &QmlApp::loginSucceeded);
        QVERIFY(app.login("13900139099"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 15000);
        QCOMPARE(accepted.size(), 0);
        QVERIFY(!app.loggedIn());
        QVERIFY(app.currentUser().isEmpty());
    }

    void exactIdsAndUtc()
    {
        charging::model::User user;
        user.id = Q_INT64_C(9007199254740993);
        QCOMPARE(marshalling::userToMap(user).value("id").toString(), QString("9007199254740993"));
        charging::model::Order order;
        order.id = user.id;
        order.createdAtUtc = QDateTime::fromString("2026-09-08T01:02:03.456Z", Qt::ISODateWithMs);
        const QVariantMap mapped = marshalling::orderToMap(order);
        QCOMPARE(mapped.value("id").metaType().id(), QMetaType::QString);
        QCOMPARE(mapped.value("createdAt").toString(), QString("2026-09-08T01:02:03.456Z"));
    }

    void clientPagesShareDatabaseWithAdmin()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        ServerRuntime runtime;
        QVERIFY(runtime.start(temp.filePath("delivery.sqlite"), true, QHostAddress::LocalHost, 0));
        QTRY_VERIFY(runtime.isListening());
        AdminRequestGateway gateway(&runtime);
        QVERIFY(admin(gateway, "auth.login", {{"username", "admin"}, {"password", "123456"}})
                    .value("success").toBool());
        QmlApp app("127.0.0.1", runtime.serverPort(), false);
        QSignalSpy login(&app, &QmlApp::loginSucceeded);
        QSignalSpy failed(&app, &QmlApp::loginFailed);
        QVERIFY(!app.login("123"));
        QCOMPARE(failed.size(), 1);
        QVERIFY(app.login("13900139019"));
        QTRY_COMPARE(login.size(), 1);
        QVERIFY(app.loggedIn());
        QCOMPARE(app.currentUser().value("nickname").toString(), QString("用户9019"));
        const QString userId = app.currentUser().value("id").toString();
        QTRY_VERIFY(!app.checkingOrders());

        // Invoke the actual QML recharge button, not a mock transport.
        QQmlEngine engine;
        bind(engine, app);
        QQuickWindow window;
        window.resize(420, 860);
        window.show();
        auto* recharge = page(engine, "RechargePage.qml", window);
        QVERIFY(recharge);
        auto* field = recharge->findChild<QObject*>("rechargeAmountField");
        auto* confirm = recharge->findChild<QObject*>("rechargeConfirmButton");
        QVERIFY(field); QVERIFY(confirm);
        field->setProperty("text", "100");
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QTRY_COMPARE(app.currentUser().value("balanceCents").toLongLong(), qint64(10000));
        delete recharge;
        auto adminUser = admin(gateway, "users.get", {{"id", userId}}).value("data").toObject()
                             .value("item").toObject();
        QCOMPARE(adminUser.value("balanceCents").toInteger(), qint64(10000));

        // Uploaded avatar and nickname survive a different TCP login.
        QImage image(48, 48, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        const QString imagePath = temp.filePath("avatar.png");
        QVERIFY(image.save(imagePath));
        const QString avatar = app.prepareAvatar(imagePath);
        QVERIFY(avatar.startsWith("data:image/png;base64,"));
        auto* wallet = qobject_cast<WalletBridge*>(app.walletService());
        QVERIFY(wallet);
        wallet->updateAvatar(avatar);
        QTRY_COMPARE(app.currentUser().value("avatarKey").toString(), avatar);
        wallet->updateNickname("真实QML用户");
        QTRY_COMPARE(app.currentUser().value("nickname").toString(), QString("真实QML用户"));

        auto* stations = qobject_cast<StationQueryBridge*>(app.stationQueryService());
        QSignalSpy stationList(stations, &StationQueryBridge::querySucceeded);
        stations->search();
        QTRY_VERIFY(!stationList.isEmpty());
        const auto station = stationList.first().at(0).toList().first().toMap();
        QSignalSpy details(stations, &StationQueryBridge::detailSucceeded);
        stations->fetchDetailById(station.value("id"), -1);
        QTRY_VERIFY(!details.isEmpty());
        QVariantMap charger;
        for (const auto& value : details.first().at(0).toMap().value("chargers").toList()) {
            if (value.toMap().value("status") == "available") { charger = value.toMap(); break; }
        }
        QVERIFY(!charger.isEmpty());
        auto* reservation = qobject_cast<ReservationBridge*>(app.reservationService());
        QSignalSpy reserved(reservation, &ReservationBridge::submitSucceeded);
        reservation->submit({{"chargerId", charger.value("id")}, {"stationId", station.value("id")},
            {"stationName", station.value("name")}, {"chargerCode", charger.value("code")},
            {"chargerPowerWatts", charger.value("powerWatts")}});
        QTRY_COMPARE(reserved.size(), 1);
        const auto reservationMap = reserved.first().first().toMap();
        QVERIFY(reservationMap.value("expiresAtUtc").toString().endsWith('Z'));
        auto* charging = qobject_cast<ChargingBridge*>(app.chargingService());
        QSignalSpy started(charging, &ChargingBridge::startCompleted);
        charging->startCharging(reservationMap.value("id"));
        QTRY_COMPARE(started.size(), 1);
        const QVariantMap startedOrder = started.first().first().toMap();
        const QString orderId = startedOrder.value("id").toString();

        // Logout / login reconstructs a fresh graph and restores authoritative charging.
        app.logout();
        QVERIFY(!app.loggedIn());
        QVERIFY(app.currentUser().isEmpty());
        QSignalSpy routes(&app, &QmlApp::navigateRequested);
        QVERIFY(app.login("13900139019"));
        QTRY_VERIFY(app.loggedIn());
        QTRY_VERIFY(!app.checkingOrders());
        QVERIFY(!routes.isEmpty());
        QCOMPARE(routes.last().at(0).toString(), QString("charging_run"));
        QCOMPARE(app.currentUser().value("avatarKey").toString(), avatar);
        bind(engine, app);
        charging = qobject_cast<ChargingBridge*>(app.chargingService());
        auto* chargingPage = page(engine, "ChargingPage.qml", window, startedOrder);
        QVERIFY(chargingPage);
        QSignalSpy stopped(charging, &ChargingBridge::stopCompleted);
        QTest::qWait(1100); // At least one measured second, hence a nonzero bill.
        auto* stopButton = chargingPage->findChild<QObject*>("stopChargingBar");
        QVERIFY(stopButton);
        QVERIFY(QMetaObject::invokeMethod(stopButton, "clicked"));
        QTRY_COMPARE(stopped.size(), 1);
        const QVariantMap stoppedOrder = stopped.first().first().toMap();
        QVERIFY(stoppedOrder.value("amountCents").toLongLong() > 0);
        delete chargingPage;
        // A fresh reservation must route to the outstanding bill, not confirmation.
        routes.clear();
        app.checkBeforeReservation({{"chargerId", charger.value("id")}});
        QTRY_VERIFY(!app.checkingOrders());
        QCOMPARE(routes.last().at(0).toString(), QString("settlement"));
        auto* settlement = page(engine, "SettlementPage.qml", window, stoppedOrder);
        QVERIFY(settlement);
        auto* pay = settlement->findChild<QObject*>("settlementPayBar");
        QVERIFY(pay);
        QSignalSpy paid(charging, &ChargingBridge::paymentCompleted);
        QVERIFY(QMetaObject::invokeMethod(pay, "clicked"));
        QTRY_COMPARE(paid.size(), 1);
        delete settlement;
        const qint64 expectedBalance = 10000 - stoppedOrder.value("amountCents").toLongLong();
        QCOMPARE(app.currentUser().value("balanceCents").toLongLong(), expectedBalance);
        const auto adminOrder = admin(gateway, "orders.get", {{"id", orderId}}).value("data").toObject()
                                    .value("item").toObject();
        QCOMPARE(adminOrder.value("status").toString(), QString("COMPLETED"));
        QCOMPARE(adminOrder.value("amountCents").toInteger(), stoppedOrder.value("amountCents").toLongLong());
        adminUser = admin(gateway, "users.get", {{"id", userId}}).value("data").toObject()
                        .value("item").toObject();
        QCOMPARE(adminUser.value("balanceCents").toInteger(), expectedBalance);
        const auto adminPile = admin(gateway, "chargers.get", {{"id", charger.value("id").toString()}})
                                   .value("data").toObject().value("item").toObject();
        QCOMPARE(adminPile.value("status").toString(), QString("AVAILABLE"));

        // Frozen users are rejected by the real server, not client-only UI rules.
        QVERIFY(admin(gateway, "user.status", {{"id", userId}, {"status", "FROZEN"},
            {"operationId", "qml-freeze"}, {"expectedUpdatedAt", adminUser.value("updatedAt")}})
                    .value("success").toBool());
        app.logout();
        failed.clear();
        QVERIFY(app.login("13900139019"));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!app.loggedIn());
        QVERIFY(app.login("13900139020"));
        QTRY_VERIFY(app.loggedIn());
        QCOMPARE(app.currentUser().value("balanceCents").toLongLong(), qint64(0));
        QVERIFY(app.currentUser().value("avatarKey").toString().isEmpty());
        // A pending request from user B is invalidated before disconnect/login C.
        qobject_cast<WalletBridge*>(app.walletService())->fetchProfile();
        app.logout();
        QVERIFY(app.login("13900139021"));
        QTRY_VERIFY(app.loggedIn());
        QTest::qWait(200);
        QCOMPARE(app.currentUser().value("phone").toString(), QString("13900139021"));
        runtime.stop();
        QTRY_VERIFY(!app.loggedIn());
        QVERIFY(app.currentUser().isEmpty());
    }
};
QTEST_MAIN(QmlTcpDeliveryTest)
#include "tst_qml_tcp_delivery.moc"
