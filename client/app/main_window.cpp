#include "main_window.h"

#include "network/client_connection.h"
#include "pages/station/login_page.h"
#include "services/station/auth_service.h"

#include <QStatusBar>

namespace charging::client {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(tr("电动汽车充电桩应用管理平台"));
    resize(720, 480);

    connection_ = new network::ClientConnection(this);
    authService_ = new services::station::AuthService(connection_, this);
    setCentralWidget(new pages::station::LoginPage(authService_, this));

    connect(connection_, &network::ClientConnection::connectionStateChanged, this,
            [this](bool connected) {
                statusBar()->showMessage(connected ? tr("已连接服务端 127.0.0.1:9527")
                                                   : tr("服务端未连接"));
            });
    statusBar()->showMessage(tr("请输入手机号登录"));
}

} // namespace charging::client
