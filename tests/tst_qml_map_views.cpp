#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <memory>

// Offline HTML checks WebEngine integration, not Tencent availability. Never
// inject production keys into the browser fixture or test diagnostics.
class MapViewBridgeFake final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasLocation READ hasLocation CONSTANT)
    Q_PROPERTY(QString locationLabel READ emptyString CONSTANT)
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(QString error READ emptyString CONSTANT)
    Q_PROPERTY(QString routeHtml MEMBER routeHtml NOTIFY changed)
    Q_PROPERTY(int routeDistanceMeters READ noDistance CONSTANT)
    Q_PROPERTY(int durationMinutes READ noDistance CONSTANT)
    Q_PROPERTY(QVariantList steps READ steps CONSTANT)
public:
    bool hasLocation() const { return false; }
    bool busy() const { return false; }
    QString emptyString() const { return {}; }
    int noDistance() const { return -1; }
    QVariantList steps() const { return {}; }
    QString routeHtml;
    Q_INVOKABLE void cancelRoute() {}
signals:
    void changed();
    void locationChanged();
};

class QmlMapViewsTest final : public QObject
{
    Q_OBJECT
    static QUrl source(const QString& name)
    {
        return QUrl::fromLocalFile(QStringLiteral(CHARGING_QML_SOURCE_DIR)
            + QStringLiteral("/pages/station/") + name);
    }
    static void run(QQmlEngine& engine, QObject* scope, const QString& script)
    {
        QQmlExpression expression(engine.rootContext(), scope, script);
        expression.evaluate();
        QVERIFY2(!expression.hasError(), qPrintable(expression.error().toString()));
    }

private slots:
    void stationMapLoadsOfflineHtmlAndEmitsSelectedStation()
    {
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(420, 300);
        QQmlComponent component(&engine, source(QStringLiteral("StationMapView.qml")));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        auto* panel = qobject_cast<QQuickItem*>(root.get());
        QVERIFY(panel);
        panel->setParentItem(window.contentItem());
        panel->setWidth(420);
        panel->setHeight(300);
        QSignalSpy selected(panel, SIGNAL(stationSelected(QString)));
        QVERIFY(selected.isValid());
        panel->setProperty("html", QStringLiteral(
            "<!doctype html><html><head><title>station-offline-ready</title></head>"
            "<body><a id='pick' style='position:absolute;left:0;top:0;width:400px;height:280px' "
            "href='charging-station://select/42'>Select station</a></body></html>"));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QObject* web = nullptr;
        QTRY_VERIFY((web = panel->findChild<QObject*>(QStringLiteral("stationWebMap"))) != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(web->property("title").toString(), QStringLiteral("station-offline-ready"), 15000);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(120, 80));
        QTRY_COMPARE(selected.count(), 1);
        QCOMPARE(selected.at(0).at(0).toString(), QStringLiteral("42"));
        QCOMPARE(web->property("title").toString(), QStringLiteral("station-offline-ready"));
        // Reject unrelated top-level navigation; station selection must not
        // replace the map with an external site or a local file.
        run(engine, web, QStringLiteral("runJavaScript(\"location.href='https://example.invalid/blocked'\")"));
        QTest::qWait(100);
        QCOMPARE(web->property("title").toString(), QStringLiteral("station-offline-ready"));
    }

    void navigationLoadsOfflineRouteAndUnchangedNotificationsPreserveDocument()
    {
        MapViewBridgeFake bridge;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("mapBridge"), &bridge);
        QQuickWindow window;
        window.resize(420, 740);
        QQmlComponent component(&engine, source(QStringLiteral("NavigationPage.qml")));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        auto* page = qobject_cast<QQuickItem*>(root.get());
        QVERIFY(page);
        page->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        bridge.routeHtml = QStringLiteral(
            "<!doctype html><html><head><title>route-offline-ready</title></head>"
            "<body>Offline route renderer fixture</body></html>");
        emit bridge.changed();
        QObject* web = nullptr;
        QTRY_VERIFY((web = page->findChild<QObject*>(QStringLiteral("navigationMapPanel"))) != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(web->property("title").toString(), QStringLiteral("route-offline-ready"), 15000);
        run(engine, web, QStringLiteral("runJavaScript(\"document.title='route-document-preserved'\")"));
        QTRY_COMPARE(web->property("title").toString(), QStringLiteral("route-document-preserved"));
        emit bridge.changed();
        emit bridge.changed();
        QTest::qWait(100);
        QCOMPARE(web->property("title").toString(), QStringLiteral("route-document-preserved"));
        bridge.routeHtml.clear();
        emit bridge.changed();
        QTRY_VERIFY(page->findChild<QObject*>(QStringLiteral("navigationMapPanel")) == nullptr);
    }
};

int main(int argc, char** argv)
{
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QmlMapViewsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qml_map_views.moc"
