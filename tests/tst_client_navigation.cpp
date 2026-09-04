#include "main_window.h"

#include "charging_server.h"
#include "database_connection.h"
#include "pages/station/home_shell.h"
#include "pages/station/login_page.h"
#include "pages/station/station_home_page.h"
#include "request_dispatcher.h"
#include "user_repository.h"
#include "user_service.h"

#include <QDir>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

// 设置 CHARGING_SNAPSHOT_DIR 时保存页面截图，用于 PR 的 UI 评审证据。
void saveSnapshotIfRequested(QWidget& widget, const QString& fileName)
{
    const QByteArray snapshotDir = qgetenv("CHARGING_SNAPSHOT_DIR");
    if (snapshotDir.isEmpty()) {
        return;
    }
    const QString directory = QString::fromLocal8Bit(snapshotDir);
    QDir().mkpath(directory);
    widget.grab().save(directory + QStringLiteral("/") + fileName);
}

class NavigationServerFixture final
{
public:
    bool start(QString* errorMessage)
    {
        if (!temporaryDirectory_.isValid()) {
            *errorMessage = QStringLiteral("Unable to create the temporary directory");
            return false;
        }
        const QString databasePath =
            temporaryDirectory_.filePath(QStringLiteral("navigation-test.sqlite3"));
        if (!database_.open(databasePath, false, errorMessage)) {
            return false;
        }

        repository_ = std::make_unique<charging::server::UserRepository>(database_.database());
        service_ = std::make_unique<charging::server::UserService>(repository_.get());
        dispatcher_ = std::make_unique<charging::server::RequestDispatcher>(service_.get());
        server_ = std::make_unique<charging::server::ChargingServer>();
        server_->setRequestDispatcher(dispatcher_.get());
        if (!server_->listen(QHostAddress::LocalHost, 0)) {
            *errorMessage = server_->errorString();
            return false;
        }
        return true;
    }

    quint16 port() const
    {
        return server_->serverPort();
    }

private:
    QTemporaryDir temporaryDirectory_;
    charging::server::DatabaseConnection database_;
    std::unique_ptr<charging::server::UserRepository> repository_;
    std::unique_ptr<charging::server::UserService> service_;
    std::unique_ptr<charging::server::RequestDispatcher> dispatcher_;
    std::unique_ptr<charging::server::ChargingServer> server_;
};

} // namespace

class ClientNavigationTest final : public QObject
{
    Q_OBJECT

private slots:
    void loginSuccessShowsHomeShell();
};

void ClientNavigationTest::loginSuccessShowsHomeShell()
{
    NavigationServerFixture fixture;
    QString startError;
    QVERIFY2(fixture.start(&startError), qPrintable(startError));

    charging::client::MainWindow window(QStringLiteral("127.0.0.1"), fixture.port());
    window.show();

    auto* phoneLineEdit = window.findChild<QLineEdit*>(QStringLiteral("phoneLineEdit"));
    auto* loginButton = window.findChild<QPushButton*>(QStringLiteral("loginButton"));
    QVERIFY(phoneLineEdit != nullptr);
    QVERIFY(loginButton != nullptr);

    // 触发点击前先让窗口完成离屏布局，避免坐标落在未映射控件上。
    QTest::qWait(20);
    phoneLineEdit->setText(QStringLiteral("13900000001"));
    saveSnapshotIfRequested(window, QStringLiteral("client_login_page.png"));
    loginButton->click();

    // 登录成功后 MainWindow 应把页面栈切到 HomeShell。
    QTRY_VERIFY_WITH_TIMEOUT(
        window.findChild<charging::client::pages::station::HomeShell*>() != nullptr, 5000);
    auto* homeShell = window.findChild<charging::client::pages::station::HomeShell*>();

    auto* pageStack = window.findChild<QStackedWidget*>(QStringLiteral("mainPageStack"));
    QVERIFY(pageStack != nullptr);
    QCOMPARE(pageStack->currentWidget(), homeShell);

    // 顶部导航公共组件已就位：搜索框 + 头像（登录态透传）。
    auto* searchLineEdit = homeShell->findChild<QLineEdit*>(QStringLiteral("navSearchLineEdit"));
    auto* avatarButton = homeShell->findChild<QPushButton*>(QStringLiteral("navAvatarButton"));
    QVERIFY(searchLineEdit != nullptr);
    QVERIFY(avatarButton != nullptr);
    QVERIFY(!avatarButton->isHidden());

    // 任务 #7：登录后找站页自动检索（模拟通道），最终渲染站点卡片列表。
    auto* stationPage =
        homeShell->findChild<charging::client::pages::station::StationHomePage*>();
    QVERIFY(stationPage != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(
        stationPage->viewState()
            == charging::client::pages::station::StationHomePage::ViewState::List,
        5000);
    QVERIFY(stationPage->stationCardCount() > 0);

    // 点击头像 → 个人中心（“我的”Tab），账户信息在此透传展示。
    avatarButton->click();
    auto* nicknameLabel = homeShell->findChild<QLabel*>(QStringLiteral("nicknameLabel"));
    auto* balanceLabel = homeShell->findChild<QLabel*>(QStringLiteral("balanceLabel"));
    QVERIFY(nicknameLabel != nullptr);
    QVERIFY(balanceLabel != nullptr);
    QVERIFY(nicknameLabel->text().contains(QStringLiteral("用户0001")));
    QVERIFY(balanceLabel->text().contains(QStringLiteral("0.00")));

    saveSnapshotIfRequested(window, QStringLiteral("client_after_login.png"));

    // 底部 4 Tab 应完整可见，默认激活“找站”。
    auto* stationTab = homeShell->findChild<QPushButton*>(QStringLiteral("tab_station"));
    auto* orderTab = homeShell->findChild<QPushButton*>(QStringLiteral("tab_order"));
    auto* chargingTab = homeShell->findChild<QPushButton*>(QStringLiteral("tab_charging"));
    auto* profileTab = homeShell->findChild<QPushButton*>(QStringLiteral("tab_profile"));
    QVERIFY(stationTab != nullptr);
    QVERIFY(orderTab != nullptr);
    QVERIFY(chargingTab != nullptr);
    QVERIFY(profileTab != nullptr);
    QVERIFY(orderTab->isVisible());

    // 回到“我的”并点“退出登录”应返回登录页，并重置为初始可输入状态。
    auto* loginPage = window.findChild<charging::client::pages::station::LoginPage*>();
    QVERIFY(loginPage != nullptr);
    auto* logoutButton = homeShell->findChild<QPushButton*>(QStringLiteral("logoutButton"));
    QVERIFY(logoutButton != nullptr);
    logoutButton->click();

    QCOMPARE(pageStack->currentWidget(), loginPage);
    auto* resultLabel = loginPage->findChild<QLabel*>(QStringLiteral("resultLabel"));
    QVERIFY(resultLabel != nullptr);
    QVERIFY(resultLabel->text().contains(QStringLiteral("请输入11位手机号")));

    saveSnapshotIfRequested(window, QStringLiteral("client_back_to_login.png"));
}

QTEST_MAIN(ClientNavigationTest)

#include "tst_client_navigation.moc"
