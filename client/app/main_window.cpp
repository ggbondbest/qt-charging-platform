#include "main_window.h"

#include "network/client_connection.h"
#include "pages/station/home_shell.h"
#include "pages/station/login_page.h"
#include "pages/station/platform_theme.h"
#include "services/station/auth_service.h"

#include <QGuiApplication>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>

namespace charging::client {

namespace {

const QString kDefaultServerHost = QStringLiteral("127.0.0.1");
constexpr quint16 kDefaultServerPort = 9527;

// 稳定尺寸页面栈：QStackedLayout 的 sizeHint/minimumSize 取**所有页的最大值**
// （Qt 设计如此），且页面数据异步加载后 updateGeometry() 会向上传播——导致
// 切换页面/登录后窗口被最大页的 min 强制撑大、退出时又弹回，桌面演示时明显
// 跳动。覆写这两个虚函数把中心件对 QMainWindow 布局报告的尺寸钉在固定手机上
// 视口，页面自身内容变化不再驱动窗口尺寸；页内长内容本就应落在滚动容器里。
class StableSizeStack : public QStackedWidget
{
public:
    using QStackedWidget::QStackedWidget;

    QSize sizeHint() const override { return QSize(kViewportWidth, kViewportHeight); }
    QSize minimumSizeHint() const override
    {
        // 仍允许演示时自由缩放到合理下限，不至于卡死窗口管理器。
        return {kViewportWidth - 60, kViewportHeight - 340};
    }

private:
    static constexpr int kViewportWidth = 420;
    static constexpr int kViewportHeight = 860;
};

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

    // 版本叙事：v0.7 预约改版（成员 2）→ v0.8 全端整合（成员 3 页面并入壳层）。
    setWindowTitle(tr("电动汽车充电桩应用管理平台 v0.8 · 全端整合"));
    // 移动端优先：默认按主流手机视口尺寸打开（预览/截图同为 420×860）；
    // 仍可自由缩放，桌面演示不受限。屏幕可视高度不足时按可用区收钳，
    // 保证底部 Tab 栏与页面底部操作按钮（如"确认预约"）始终可达。
    int preferredHeight = 860;
    if (const auto* screen = QGuiApplication::primaryScreen()) {
        preferredHeight = qMin(preferredHeight, screen->availableGeometry().height() - 48);
    }
    // 下限交给 Qt 的 minimumSizeHint 兜底（布局自身保证可达）。
    resize(420, preferredHeight);

    connection_ = new network::ClientConnection(hostName, port, this);
    authService_ = new services::station::AuthService(connection_, this);

    pageStack_ = new StableSizeStack(this);
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
                if (!connected && homeShell_ != nullptr) showLoginPage();
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
        homeShell_ = new pages::station::HomeShell(user, connection_, pageStack_);
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
    if (homeShell_ != nullptr) {
        pageStack_->removeWidget(homeShell_);
        homeShell_->deleteLater();
        homeShell_ = nullptr;
    }
    connection_->disconnectFromServer();
    pageStack_->setCurrentWidget(loginPage_);
    loginPage_->resetState();
    statusBar()->show();
    statusBar()->showMessage(tr("请输入手机号登录"));
}

} // namespace charging::client
