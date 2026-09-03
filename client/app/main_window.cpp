#include "main_window.h"

#include "network/client_connection.h"
#include "pages/station/home_shell.h"
#include "pages/station/login_page.h"
#include "pages/station/platform_theme.h"
#include "services/station/auth_service.h"

#include <QStackedWidget>
#include <QStatusBar>

namespace charging::client {

namespace {

const QString kDefaultServerHost = QStringLiteral("127.0.0.1");
constexpr quint16 kDefaultServerPort = 9527;

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : MainWindow(kDefaultServerHost, kDefaultServerPort, parent)
{
}

MainWindow::MainWindow(const QString& hostName, quint16 port, QWidget* parent)
    : QMainWindow(parent)
{
    // 平台主题（成员 3 维护的全局 QSS token）在装配任何页面前安装一次。
    pages::station::installPlatformTheme();

    setWindowTitle(tr("电动汽车充电桩应用管理平台 v0.5 · 站点详情"));
    resize(760, 600);

    connection_ = new network::ClientConnection(hostName, port, this);
    authService_ = new services::station::AuthService(connection_, this);

    pageStack_ = new QStackedWidget(this);
    pageStack_->setObjectName(QStringLiteral("mainPageStack"));
    pageStack_->setStyleSheet(
        QStringLiteral("#mainPageStack { background: #F4F6F8; }"));
    loginPage_ = new pages::station::LoginPage(authService_, pageStack_);
    pageStack_->addWidget(loginPage_);
    setCentralWidget(pageStack_);

    connect(authService_, &services::station::AuthService::loginSucceeded, this,
            &MainWindow::showHomePage);
    connect(connection_, &network::ClientConnection::connectionStateChanged, this,
            [this](bool connected) {
                statusBar()->showMessage(
                    connected ? tr("已连接服务端 %1:%2")
                                    .arg(connection_->hostName())
                                    .arg(connection_->port())
                              : tr("服务端未连接"));
            });
    statusBar()->showMessage(tr("请输入手机号登录"));
}

void MainWindow::showHomePage(const charging::model::User& user)
{
    if (homeShell_ == nullptr) {
        homeShell_ = new pages::station::HomeShell(user, pageStack_);
        // 真实站点接口（服务端 GET_STATIONS）就绪后，仅需在此开启 liveMode，
        // 页面 UI 逻辑零改动；当前保持模拟数据通道渲染。
        homeShell_->setConnection(connection_);
        pageStack_->addWidget(homeShell_);
        connect(homeShell_, &pages::station::HomeShell::logoutRequested, this,
                &MainWindow::showLoginPage);
        // 未登录壳的顶部“登录”按钮 → 进入登录页。
        connect(homeShell_, &pages::station::HomeShell::loginRequested, this,
                &MainWindow::showLoginPage);
    }
    pageStack_->setCurrentWidget(homeShell_);
    statusBar()->hide();
}

void MainWindow::showLoginPage()
{
    pageStack_->setCurrentWidget(loginPage_);
    loginPage_->resetState();
    statusBar()->show();
    statusBar()->showMessage(tr("请输入手机号登录"));
}

} // namespace charging::client
