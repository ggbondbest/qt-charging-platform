#include "admin_login_page.h"
#include "admin_request_gateway.h"
#include "main_window.h"
#include "server_runtime.h"

#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
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
        window.adminGateway()->logout();
        QVERIFY(page->isVisible());
        QVERIFY(!window.adminGateway()->isAuthenticated());
        runtime.stop();
    }
};
QTEST_MAIN(AdminLoginTest)
#include "tst_admin_login.moc"
