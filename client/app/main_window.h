#pragma once

#include <QMainWindow>

namespace charging::client::network {
class ClientConnection;
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

private:
    network::ClientConnection* connection_ = nullptr;
    services::station::AuthService* authService_ = nullptr;
};

} // namespace charging::client
