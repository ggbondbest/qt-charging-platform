#pragma once

#include <QMainWindow>
#include <QString>

class QStackedWidget;

namespace charging::model {
struct User;
}

namespace charging::client::network {
class ClientConnection;
}

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
    QStackedWidget* pageStack_ = nullptr;
    pages::station::LoginPage* loginPage_ = nullptr;
    pages::station::HomeShell* homeShell_ = nullptr;
};

} // namespace charging::client
