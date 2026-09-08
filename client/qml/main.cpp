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
#include <QJsonDocument>
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
    // --arg=JSON: deep-link route parameter (map for detail/confirm pages,
    // bare string for keyword args). Falls back to plain string if not JSON.
    QVariant deepArg;
    const QString argRaw = valueOf("--arg=");
    if (!argRaw.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(argRaw.toUtf8());
        deepArg = doc.isNull() ? QVariant(argRaw) : QVariant(doc.toVariant());
    }
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
    ctx->setContextProperty(QStringLiteral("chargingArg"), deepArg);
    ctx->setContextProperty(QStringLiteral("App"), &qmlApp);
    // Contract §1 channel switch (tcp wiring is TODO(contract) — mock only now).
    ctx->setContextProperty(QStringLiteral("CHARGING_CHANNEL"),
                            qEnvironmentVariable("CHARGING_CHANNEL", "mock"));
    // Screenshot/demo convenience: deep links past the login gate ride the
    // demo account in. "login"/"station" keep the gate for the real flow
    // unless --logged-in is passed (lets shots exercise post-login routes).
    if ((view != QLatin1String("login") && view != QLatin1String("station"))
        || app.arguments().contains(QStringLiteral("--logged-in")))
        qmlApp.login(QStringLiteral("13800138000"));
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
    ctx->setContextProperty(QStringLiteral("statsService"), qmlApp.statsService());
    ctx->setContextProperty(QStringLiteral("couponService"), qmlApp.couponService());
    ctx->setContextProperty(QStringLiteral("pointsService"), qmlApp.pointsService());
    ctx->setContextProperty(QStringLiteral("ratingsService"), qmlApp.ratingsService());
    ctx->setContextProperty(QStringLiteral("authService"), qmlApp.authService());
    // --theme=light|dark / --font=standard|large|extraLarge（批次A）：覆盖式
    // 设置外观持久化（Service 白名单自拒非法值），Shell 启动同步即生效——
    // 给暗色截图验收与演示用；不传则维持本机已存值。
    const QString themeArg = valueOf("--theme=");
    if (!themeArg.isEmpty())
        QMetaObject::invokeMethod(qmlApp.settingsService(), "setTheme",
                                  Q_ARG(QString, themeArg));
    const QString fontArg = valueOf("--font=");
    if (!fontArg.isEmpty())
        QMetaObject::invokeMethod(qmlApp.settingsService(), "setFontScale",
                                  Q_ARG(QString, fontArg));
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
        // 1100ms covers two 450ms mock round-trips (deep-link fallback chains).
        QTimer::singleShot(1100, window, [window, shot]() {
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
