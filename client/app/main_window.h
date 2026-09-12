#pragma once

#include <QMainWindow>
#include <QString>

// 文件职责：MainWindow 声明——widgets 通道的应用窗口骨架，只管“登录页 ⇄ 首页壳”
// 的切换与唯一 TCP 连接/认证服务的生命周期，具体装配逻辑全部在 main_window.cpp。
// 前向声明风格：头文件里只以指针出现的全类型，在此声明即可、完整头归 cpp 包含，
// 压低头文件耦合（改页面实现不触发本头重编）。
class QStackedWidget;

namespace charging::model {
struct User;
}

namespace charging::client::network {
class ClientConnection;
}

// 本窗口负责装配的两个页面：LoginPage 构造即入栈，HomeShell 登录成功后才懒建。
namespace charging::client::pages::station {
class HomeShell;
class LoginPage;
}

namespace charging::client::services::station {
class AuthService;
}

namespace charging::client {

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // 指定服务端地址/端口；默认 127.0.0.1:9527。测试或部署到自定义地址时使用。
    explicit MainWindow(const QString& hostName, quint16 port, QWidget* parent = nullptr);

private:
    void showHomePage(const charging::model::User& user);
    void showLoginPage();

    network::ClientConnection* connection_ = nullptr;
    services::station::AuthService* authService_ = nullptr;
    // pageStack_/loginPage_ 交 Qt 父子所有权托管；homeShell_ 以“空指针=不在首页”
    // 为约定：登录懒建、登出 deleteLater 后清零，避免悬垂指针再被切页引用。
    QStackedWidget* pageStack_ = nullptr;
    pages::station::LoginPage* loginPage_ = nullptr;
    pages::station::HomeShell* homeShell_ = nullptr;
};

} // namespace charging::client
