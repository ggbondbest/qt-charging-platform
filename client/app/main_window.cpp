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

// 默认接入参数：与服务端 server/app/main.cpp 的默认监听端口同口径（9527），
// 本地演示零配置直连；换地址/端口走下面的带参构造（测试与部署用）。
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

// ---- MainWindow：widgets 通道的应用入口装配层 ----
// 无参构造委托带参构造，把默认 127.0.0.1:9527 钉在唯一一处。
MainWindow::MainWindow(QWidget* parent)
    : MainWindow(kDefaultServerHost, kDefaultServerPort, parent)
{
}

// 构造只做“装骨架”：全局主题 → 窗口标题/视口尺寸 → TCP 连接 + 认证服务 →
// 页栈（登录页起步）→ 两条总线信号（登录成功进壳、断链踢回登录页）。
// 各业务页面不在这里 new，全部归 HomeShell / LoginPage 内部自行装配。
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
    // 默认高 860 与 StableSizeStack::kViewportHeight 同值：窗口初始尺寸即页栈报告
    // 尺寸，首帧无二次调整；-48 给窗口标题栏/边框留经验余量，保证整窗落在可用区内。
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
    // 页栈初始只装登录页：HomeShell 首次登录成功才懒建入栈，登出时移除销毁。
    loginPage_ = new pages::station::LoginPage(authService_, pageStack_);
    pageStack_->addWidget(loginPage_);
    setCentralWidget(pageStack_);

    // 两条总线信号：登录成功 → 进首页壳；服务端断连 → 退回登录页并在状态栏提示。
    // 断连回退仅在 homeShell_ 已存在时触发，避免未登录时的连接失败弹回空壳。
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

// 登录成功进壳：HomeShell 懒建——只在新建时才入栈并接登出/登录回退信号；
// 重复触发（如二次登录成功）直接复用已有壳，不重建，User 由壳构造注入。
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
    // 登出收尾：切回登录页并 resetState 清掉上次表单/提示，状态栏重新挂出作
    // “登录前”语境提示（进壳时被 hide()，此处 show() 还原）。
    pageStack_->setCurrentWidget(loginPage_);
    loginPage_->resetState();
    statusBar()->show();
    statusBar()->showMessage(tr("请输入手机号登录"));
}

} // namespace charging::client
