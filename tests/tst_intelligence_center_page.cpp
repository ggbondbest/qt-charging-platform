#include "intelligence_center_page.h"
#include "dashboard_service_manager.h"

#include <QDesktopServices>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#ifdef CHARGING_EMBED_WEB_DASHBOARD
#include <QWebEnginePage>
#include <QWebEngineView>
#endif

using charging::server::IntelligenceCenterPage;
using charging::server::DashboardServiceManager;

class IntelligenceCenterPageTest final : public QObject
{
    Q_OBJECT
    QUrl opened_;
private slots:
    void init() { opened_.clear(); }
    void captureUrl(const QUrl& url) { opened_ = url; }
    void normalizesModeWithoutLosingApplicationQuery()
    {
        QString error;
        const auto embedded = IntelligenceCenterPage::dashboardUrl(
            "http://127.0.0.1:8000/?city=Beijing&embedded=old&embedded=other#live", true, &error);
        QVERIFY(error.isEmpty());
        const QUrlQuery query(embedded);
        QCOMPARE(query.allQueryItemValues("embedded"), QStringList{"operator"});
        QCOMPARE(query.queryItemValue("city"), QString("Beijing"));
        QCOMPARE(embedded.fragment(), QString("live"));
        const auto browser = IntelligenceCenterPage::dashboardUrl(embedded.toString(), false);
        QVERIFY(!QUrlQuery(browser).hasQueryItem("embedded"));
        QCOMPARE(browser.host(), QString("127.0.0.1"));
        QCOMPARE(browser.port(), 8000);
    }
    void rejectsInvalidAndNonWebAddresses_data()
    {
        QTest::addColumn<QString>("input");
        for (const auto& input : {"", "localhost:8000", "file:///tmp/index.html", "javascript:alert(1)",
                                  "https://admin:password@example.com/", "http:///broken"})
            QTest::newRow(input) << QString::fromUtf8(input);
    }
    void rejectsInvalidAndNonWebAddresses()
    {
        QFETCH(QString, input);
        QString error;
        QVERIFY(IntelligenceCenterPage::dashboardUrl(input, true, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void invalidAddressShowsRecoveryWithoutCreatingWebView()
    {
        IntelligenceCenterPage page;
        auto* retry = page.findChild<QPushButton*>("dashboardReloadButton");
        auto* status = page.findChild<QLabel*>("dashboardStatus");
        QVERIFY(retry && status);
        page.setDashboardAddress("file:///tmp/private");
        page.show();
        QVERIFY(status->text().contains("地址不可用"));
        QVERIFY(retry->isVisible());
        QVERIFY(!page.findChild<QLineEdit*>());
        QVERIFY(!page.findChild<QWidget*>("dashboardWebView"));
    }
    void managedStartupAndFailureKeepBusinessPageRecoverable()
    {
        DashboardServiceManager::Options options;
        options.configurationError = QStringLiteral("网页运行环境尚未配置，请重试。");
        DashboardServiceManager manager(options);
        IntelligenceCenterPage page;
        page.setServiceManager(&manager);
        auto* retry = page.findChild<QPushButton*>("dashboardReloadButton");
        page.show();
        QVERIFY(!page.findChild<QLineEdit*>());
        QVERIFY(!page.findChild<QPushButton*>("dashboardBrowserButton"));
        QVERIFY(!retry->isVisible());
        QVERIFY(!page.findChild<QWidget*>("dashboardWebView"));
        manager.start(QString());
        QCOMPARE(manager.state(), DashboardServiceManager::State::Failed);
        QCOMPARE(retry->text(), QString("重试"));
        QVERIFY(retry->isEnabled());
        QVERIFY(page.findChild<QLabel*>("dashboardStatus")->text().contains("尚未配置"));
        QSignalSpy changes(&manager, &DashboardServiceManager::stateChanged);
        retry->click();
        QVERIFY(!changes.isEmpty());
        QVERIFY(!page.findChild<QWidget*>("dashboardWebView"));
    }
#ifdef CHARGING_EMBED_WEB_DASHBOARD
    void embeddedLoadAndUnavailableServiceRecovery()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    if (!socket->canReadLine()) return;
                    socket->readAll();
                    const QByteArray body("<!doctype html><title>Operations test</title><p>Qt live</p>"
                        "<button style='position:fixed;left:20px;top:20px;width:200px;height:60px' "
                        "onclick='document.documentElement.requestFullscreen().catch(error=>document.title=String(error))'>Fullscreen</button>"
                        "<script>document.addEventListener('fullscreenchange',()=>{document.title="
                        "document.fullscreenElement?'Fullscreen active':'Fullscreen ended'});</script>");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        IntelligenceCenterPage page;
        DashboardServiceManager::Options options;
        options.externalUrl = QString("http://127.0.0.1:%1/").arg(server.serverPort());
        DashboardServiceManager manager(options);
        page.setServiceManager(&manager);
        page.resize(1024, 720);
        page.show();
        QVERIFY(!page.findChild<QWidget*>("dashboardWebView"));
        manager.start(QString());
        QTRY_VERIFY_WITH_TIMEOUT(page.findChild<QWidget*>("dashboardWebView")
                                 && page.findChild<QWidget*>("dashboardWebView")->isVisible(), 15000);
        auto* view = page.findChild<QWebEngineView*>("dashboardWebView");
        QCOMPARE(view->size(), page.size());
        QVERIFY(!page.findChild<QLabel*>("dashboardStatus")->isVisible());
        QVERIFY(!page.findChild<QPushButton*>("dashboardReloadButton")->isVisible());
        QSignalSpy fullScreen(&page, &IntelligenceCenterPage::fullScreenRequested);
        // loadFinished makes the view visible before Chromium has necessarily
        // committed its first frame/hit-test surface. Establish focus and wait
        // for rendered frames before sending the single real DOM click; merely
        // retrying a lost click would hide this Qt 6.12 offscreen race.
        view->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(view->hasFocus());
        view->page()->runJavaScript(
            "requestAnimationFrame(()=>requestAnimationFrame(()=>document.title='Input ready'))");
        QTRY_COMPARE(view->page()->title(), QString("Input ready"));
        QTest::mouseClick(view->focusProxy() ? view->focusProxy() : view, Qt::LeftButton, Qt::NoModifier, QPoint(90, 50));
        QTRY_VERIFY_WITH_TIMEOUT(page.isWebFullScreen(), 5000);
        QTRY_COMPARE(view->page()->title(), QString("Fullscreen active"));
        QCOMPARE(fullScreen.size(), 1);
        QCOMPARE(fullScreen.first().first().toBool(), true);
        page.exitFullScreen();
        QTRY_VERIFY(!page.isWebFullScreen());
        QTRY_COMPARE(view->page()->title(), QString("Fullscreen ended"));
        QCOMPARE(fullScreen.size(), 2);
        QCOMPARE(fullScreen.last().first().toBool(), false);
        server.close();
        page.setDashboardAddress(options.externalUrl);
        auto* status = page.findChild<QLabel*>("dashboardStatus");
        QTRY_VERIFY_WITH_TIMEOUT(status->isVisible() && status->text().contains("加载失败"), 15000);
        QVERIFY(page.findChild<QPushButton*>("dashboardReloadButton")->isVisible());
    }
#endif

#ifndef CHARGING_EMBED_WEB_DASHBOARD
    void opensFullPageInBrowser()
    {
        IntelligenceCenterPage page;
        page.setDashboardAddress("http://127.0.0.1:8000/?embedded=operator&city=SZ");
        page.show();
        QDesktopServices::setUrlHandler("http", this, "captureUrl");
        page.findChild<QPushButton*>("dashboardReloadButton")->click();
        QDesktopServices::unsetUrlHandler("http");
        QCOMPARE(opened_, QUrl("http://127.0.0.1:8000/?city=SZ"));
    }
#endif
};

QTEST_MAIN(IntelligenceCenterPageTest)
#include "tst_intelligence_center_page.moc"
