// Production QML client. TCP is the default; preview screenshots are opt-in.
// Connection: --host HOST --port PORT (or CHARGING_SERVER_HOST/PORT).
//   --view=NAME            sets root "view" property via context (routing)
//   --screenshot=PATH      grab after first render, then exit (CI-safe)
//   --size=WxH             default 420x860
#include <QDir>
#include <QApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QJsonDocument>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "app_bridge.h"

int main(int argc, char* argv[])
{
    QtWebEngineQuick::initialize();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("charging-client"));
    QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatform"));

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
    // Keep screenshot/deep-link flags while accepting standard endpoint flags.
    QString host = qEnvironmentVariable("CHARGING_SERVER_HOST", "127.0.0.1");
    int port = qEnvironmentVariableIntValue("CHARGING_SERVER_PORT");
    if (port <= 0) port = 9527;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == "--host" && i + 1 < argc) host = QString::fromLocal8Bit(argv[++i]);
        else if (argument == "--port" && i + 1 < argc) port = QString::fromLocal8Bit(argv[++i]).toInt();
        else if (argument.startsWith("--host=")) host = argument.mid(7);
        else if (argument.startsWith("--port=")) port = argument.mid(7).toInt();
    }
    if (host.trimmed().isEmpty() || port < 1 || port > 65535) {
        qCritical() << "Expected --host HOST --port 1..65535";
        return 2;
    }
    charging::qml::QmlApp qmlApp(host, quint16(port),
        qEnvironmentVariable("CHARGING_CHANNEL") == QStringLiteral("mock"));
    auto* ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("chargingView"), view);
    ctx->setContextProperty(QStringLiteral("chargingArg"), deepArg);
    ctx->setContextProperty(QStringLiteral("App"), &qmlApp);
    // Mock is an explicit preview option, never the production default.
    ctx->setContextProperty(QStringLiteral("CHARGING_CHANNEL"),
                            qmlApp.mockMode() ? QStringLiteral("mock") : QStringLiteral("tcp"));
    // Screenshot/demo convenience: deep links past the login gate ride the
    // demo account in. "login"/"station" keep the gate for the real flow
    // unless --logged-in is passed (lets shots exercise post-login routes).
    if (qmlApp.mockMode() && ((!view.isEmpty() && view != QLatin1String("login")
        && view != QLatin1String("station"))
        || app.arguments().contains(QStringLiteral("--logged-in"))))
        qmlApp.login(QStringLiteral("13800138000"));
    // CONTRACT.md §1: bare service names, objects pass through verbatim.
    const auto bindServices = [&qmlApp, ctx]() {
        ctx->setContextProperty(QStringLiteral("walletService"), qmlApp.walletService());
        ctx->setContextProperty(QStringLiteral("orderService"), qmlApp.orderService());
        ctx->setContextProperty(QStringLiteral("chargingService"), qmlApp.chargingService());
        ctx->setContextProperty(QStringLiteral("reservationService"), qmlApp.reservationService());
        ctx->setContextProperty(QStringLiteral("settingsService"), qmlApp.settingsService());
        ctx->setContextProperty(QStringLiteral("mapGeoService"), qmlApp.mapGeoService());
        ctx->setContextProperty(QStringLiteral("favoritesService"), qmlApp.favoritesService());
        ctx->setContextProperty(QStringLiteral("notificationService"), qmlApp.notificationService());
        ctx->setContextProperty(QStringLiteral("stationQueryService"), qmlApp.stationQueryService());
    };
    bindServices();
    QObject::connect(&qmlApp, &charging::qml::QmlApp::servicesChanged, ctx, bindServices);
    ctx->setContextProperty(QStringLiteral("mapBridge"), qmlApp.mapBridge());
    ctx->setContextProperty(QStringLiteral("authService"), qmlApp.authService());
    QQmlComponent component(&engine);
    component.loadUrl(QUrl(QStringLiteral("qrc:/charging/Shell.qml")));
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
    const int result = app.exec();
    delete window; // Pages disconnect while the service graph is still alive.
    return result;
}
