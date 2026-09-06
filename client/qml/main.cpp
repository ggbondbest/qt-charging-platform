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
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

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
    engine.rootContext()->setContextProperty(QStringLiteral("chargingView"), view);
    QQmlComponent component(&engine);
    component.loadUrl(QUrl::fromLocalFile(
        QStringLiteral(CHARGING_QML_SOURCE_DIR) + QStringLiteral("/Root.qml")));
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
        // Grab once the first frame is composited (offscreen-safe path).
        QObject::connect(window, &QQuickWindow::sceneGraphInitialized, window,
                         [window, shot]() {
                             const QImage img = window->grabWindow();
                             if (img.isNull() || !img.save(shot)) {
                                 qWarning() << "screenshot failed" << shot;
                                 QCoreApplication::exit(2);
                             } else {
                                 qInfo() << "saved" << shot;
                                 QCoreApplication::quit();
                             }
                         });
    }
    window->show();
    return app.exec();
}
