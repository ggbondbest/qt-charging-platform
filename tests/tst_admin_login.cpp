#include "admin_login_page.h"
#include "admin_request_gateway.h"
#include "main_window.h"
#include "server_runtime.h"
#include "dashboard_service_manager.h"
#include "intelligence_center_page.h"

#include <QLineEdit>
#include <QAction>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QFrame>
#include <QTemporaryDir>
#include <QtTest>

using namespace charging::server;
class AdminLoginTest final : public QObject
{
    Q_OBJECT
private slots:
    void realLoginAndLogout()
    {
        QTemporaryDir directory;
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        QVERIFY(
            runtime.start(directory.filePath("login.sqlite"), true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        MainWindow window(&runtime);
        DashboardServiceManager::Options options;
        options.autoStart = false;
        DashboardServiceManager dashboard(options);
        dashboard.start(QString());
        window.setDashboardServiceManager(&dashboard);
        // Use a compact normal window: restoreGeometry intentionally clamps
        // off-screen saved sizes to the active display's available geometry.
        window.resize(1024, 720);
        window.show();
        auto* page = window.findChild<AdminLoginPage*>();
        QVERIFY(page);
        auto* username = page->findChild<QLineEdit*>("usernameLineEdit");
        auto* password = page->findChild<QLineEdit*>("passwordLineEdit");
        auto* button = page->findChild<QPushButton*>("loginButton");
        QVERIFY(username && password && button);
        username->setText("admin");
        password->setText("wrong");
        button->click();
        QTRY_VERIFY(button->isEnabled());
        QVERIFY(page->isVisible());
        QVERIFY(!window.adminGateway()->isAuthenticated());
        password->setText("123456");
        button->click();
        QTRY_VERIFY(window.adminGateway()->isAuthenticated());
        QTRY_VERIFY(!page->isVisible());
        QVERIFY(password->text().isEmpty());
        auto* pages = window.findChild<QStackedWidget*>("managementPages");
        auto* center = window.findChild<IntelligenceCenterPage*>();
        auto* sidebar = window.findChild<QFrame*>("sidebar");
        auto* header = window.findChild<QWidget*>("managementHeader");
        auto* account = sidebar->findChild<QPushButton*>("sidebarAccountButton");
        auto* accountMenu = window.findChild<QMenu*>("sidebarAccountMenu");
        auto* signOut = window.findChild<QAction*>("sidebarLogoutAction");
        QVERIFY(pages && center && sidebar && header && account && accountMenu && signOut);
        QCOMPARE(account->text(), QString("管理员"));
        QCOMPARE(accountMenu->actions(), QList<QAction*>{signOut});
        QVERIFY(!window.findChild<QPushButton*>("sidebarLogoutButton"));
        QVERIFY(!window.findChild<QPushButton*>("sessionButton"));
        QVERIFY(header->findChildren<QPushButton*>().isEmpty());
        QCOMPARE(header->findChildren<QLabel*>().size(), 1);
        QCOMPARE(account->focusPolicy(), Qt::StrongFocus);
        QCOMPARE(pages->count(), 8);
        QCOMPARE(pages->currentWidget(), center);
        QCOMPARE(window.findChild<QPushButton*>("managementNav0")->text(), QString("运营中心"));
        QVERIFY(!window.findChild<QPushButton*>("managementNav8"));
        QVERIFY(!header->isVisible());
        QVERIFY(sidebar->isVisible());
        QVERIFY(account->isVisible());
        QVERIFY(account->mapTo(sidebar, QPoint()).y() > sidebar->height() - 90);
        window.activateWindow();
        account->setFocus();
        QTest::keyClick(account, Qt::Key_Return);
        QTRY_VERIFY(accountMenu->isVisible());
        QVERIFY(accountMenu->geometry().bottom() < account->mapToGlobal(QPoint()).y());
        QCOMPARE(accountMenu->activeAction(), signOut);
        QTest::keyClick(accountMenu, Qt::Key_Escape);
        QTRY_VERIFY(!accountMenu->isVisible());
        QVERIFY(window.adminGateway()->isAuthenticated());
        const QSize normalSize = window.size();
        // The page test exercises the real Chromium Fullscreen API; this checks
        // the production shell's window/chrome/Escape lifecycle separately.
        center->fullScreenRequested(true);
        center->fullScreenRequested(true); // repeated request must not overwrite restoration state
        QTRY_VERIFY(window.isFullScreen());
        QVERIFY(!sidebar->isVisible());
        QVERIFY(!header->isVisible());
        QTRY_COMPARE(center->size(), window.centralWidget()->size());
        window.activateWindow();
        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY(!window.isFullScreen());
        QCOMPARE(window.size(), normalSize);
        QVERIFY(sidebar->isVisible());
        for (int index = 1; index < pages->count(); ++index) {
            window.findChild<QPushButton*>(QStringLiteral("managementNav%1").arg(index))->click();
            QVERIFY(header->isVisible());
            QVERIFY(account->isVisible());
            QCOMPARE(header->findChildren<QPushButton*>().size(), 0);
        }
        window.findChild<QPushButton*>("managementNav0")->click();
        QVERIFY(!header->isVisible());
        // Space opens the same account menu; Return activates its logout action.
        account->setFocus();
        QTest::keyClick(account, Qt::Key_Space);
        QTRY_VERIFY(accountMenu->isVisible());
        QTest::keyClick(accountMenu, Qt::Key_Return);
        QTRY_VERIFY(page->isVisible());
        QVERIFY(!window.adminGateway()->isAuthenticated());
        username->setText("admin");
        password->setText("123456");
        button->click();
        QTRY_VERIFY(window.adminGateway()->isAuthenticated());
        center->fullScreenRequested(true);
        QTRY_VERIFY(window.isFullScreen());
        signOut->trigger(); // session revocation must also restore the fullscreen shell
        QVERIFY(page->isVisible());
        QVERIFY(!window.isFullScreen());
        QVERIFY(!window.adminGateway()->isAuthenticated());
        runtime.stop();
    }
};
QTEST_MAIN(AdminLoginTest)
#include "tst_admin_login.moc"
