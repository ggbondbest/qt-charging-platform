// charging-qml-preview — QML migration smoke/preview host.
// Mirrors the widgets preview CLI contract:
//   --view=NAME            sets root "view" property via context (routing)
//   --screenshot=PATH      grab after first render, then exit (CI-safe)
//   --size=WxH             default 420x860
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#include "app_bridge.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("charging-qml-preview"));

    // Basic is the only Controls style guaranteed shipped by the base module.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Hand-rolled arg parsing (same contract as the widgets preview tool), so
    // unknown flags never abort us.
    auto valueOf = [&argc, argv](const char* key) -> QString {
        const QByteArray needle = QByteArray(key);
        for (int i = 1; i < argc; ++i) {
            const QByteArray a = argv[i];
            if (a.startsWith(needle))
                return QString::fromLocal8Bit(a.mid(needle.size()));
        }
        return {};
    };

    const QString shot = valueOf("--screenshot=");
    const QString view = valueOf("--view=");
    QSize size(420, 860);
    const QString sizeArg = valueOf("--size=");
    if (!sizeArg.isEmpty()) {
        const auto parts = sizeArg.split(QLatin1Char('x'));
        if (parts.size() == 2)
            size = QSize(parts[0].toInt(), parts[1].toInt());
    }

    QQmlEngine engine;
    charging::qml::QmlApp qmlApp;
    auto* ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("chargingView"), view);
    ctx->setContextProperty(QStringLiteral("App"), &qmlApp);
    // CONTRACT.md §1: bare service names, objects pass through verbatim.
    ctx->setContextProperty(QStringLiteral("walletService"), qmlApp.walletService());
    ctx->setContextProperty(QStringLiteral("orderService"), qmlApp.orderService());
    ctx->setContextProperty(QStringLiteral("chargingService"), qmlApp.chargingService());
    ctx->setContextProperty(QStringLiteral("reservationService"), qmlApp.reservationService());
    ctx->setContextProperty(QStringLiteral("settingsService"), qmlApp.settingsService());
    ctx->setContextProperty(QStringLiteral("mapGeoService"), qmlApp.mapGeoService());
    ctx->setContextProperty(QStringLiteral("favoritesService"), qmlApp.favoritesService());
    ctx->setContextProperty(QStringLiteral("notificationService"), qmlApp.notificationService());
    ctx->setContextProperty(QStringLiteral("stationQueryService"), qmlApp.stationQueryService());
    ctx->setContextProperty(QStringLiteral("authService"), qmlApp.authService());
    QQmlComponent component(&engine);
    component.loadUrl(QUrl::fromLocalFile(
        QStringLiteral(CHARGING_QML_SOURCE_DIR) + QStringLiteral("/Shell.qml")));
    if (component.isError()) {
        qWarning().noquote() << "QML load failed:" << component.errorString();
        return 1;
    }

    QQuickWindow* window = qobject_cast<QQuickWindow*>(component.create());
    if (!window) {
        qWarning() << "Root is not a Window";
        return 1;
    }
    window->resize(size);

    if (!shot.isEmpty()) {
        // grabWindow() is dead under the offscreen platform (verified); render
        // the item tree directly instead — no platform surface required.
        QTimer::singleShot(500, window, [window, shot]() {
            auto* content = window->contentItem();
            auto result = content->grabToImage();
            QObject::connect(result.data(), &QQuickItemGrabResult::ready,
                             [result, shot]() {
                                 const QImage img = result->image();
                                 if (img.isNull() || !img.save(shot)) {
                                     qWarning() << "screenshot failed" << shot;
                                     QCoreApplication::exit(2);
                                 } else {
                                     qInfo() << "saved" << shot << img.size();
                                     QCoreApplication::quit();
                                 }
                             });
        });
    }
    window->show();
    return app.exec();
}
