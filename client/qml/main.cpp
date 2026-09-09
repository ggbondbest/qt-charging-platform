// Production QML client. TCP is the default; preview screenshots are opt-in.
// Connection: --host HOST --port PORT (or CHARGING_SERVER_HOST/PORT).
//   --view=NAME            sets root "view" property via context (routing)
//   --screenshot=PATH      grab after first render, then exit (CI-safe)
//   --size=WxH             default 420x860
#include <QDir>
#include <QFileInfo>
#include <QApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QJsonDocument>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QPalette>
#include <QSettings>
#include <QTimer>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include "app_bridge.h"
#include "glyph_provider.h"

int main(int argc, char* argv[])
{
    QtWebEngineQuick::initialize();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("charging-client"));
    QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatform"));

    // Basic is the only Controls style guaranteed shipped by the base module.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Controls not yet using the platform wrappers must still match the
    // application's light default, independent of the desktop color scheme.
    // Use Qt 6.2 palette roles only (QPalette::Accent is newer).
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#F4F6F8")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#1F2937")));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#EEF2F6")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#1F2937")));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#1F2937")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#6B7280")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#00A76D")));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, Qt::white);
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#1F2937")));
    palette.setColor(QPalette::Light, Qt::white);
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#EEF2F6")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#D5DCE4")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#9CA3AF")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#6B7280")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#9CA3AF")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#9CA3AF")));
    app.setPalette(palette);

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
    if (!shot.isEmpty()) {
        // Preview processes must not mutate the real user's appearance or
        // race each other's theme. QSettings native stores do not uniformly
        // honor XDG_CONFIG_HOME, so make screenshot isolation explicit.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                          QDir(QFileInfo(shot).absolutePath()).filePath(QStringLiteral("config")));
    }
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
    // 单色 glyph 染色（image://glyphs/<name>/<hex>）：emoji 图标替换的运行时面。
    engine.addImageProvider(QStringLiteral("glyphs"), new charging::qml::GlyphProvider);
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
    // Re-bound on servicesChanged: each login rebuilds the bridge graph, and
    // stale context pointers must never outlive their session.
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
        ctx->setContextProperty(QStringLiteral("statsService"), qmlApp.statsService());
        ctx->setContextProperty(QStringLiteral("couponService"), qmlApp.couponService());
        ctx->setContextProperty(QStringLiteral("pointsService"), qmlApp.pointsService());
        ctx->setContextProperty(QStringLiteral("ratingsService"), qmlApp.ratingsService());
    };
    bindServices();
    QObject::connect(&qmlApp, &charging::qml::QmlApp::servicesChanged, ctx, bindServices);
    ctx->setContextProperty(QStringLiteral("mapBridge"), qmlApp.mapBridge());
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
