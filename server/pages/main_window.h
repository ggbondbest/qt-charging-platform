#pragma once

#include <QMainWindow>

class QLabel;

namespace charging::server {

class ChargingServer;

class MainWindow final : public QMainWindow
{
public:
    explicit MainWindow(ChargingServer* server, QWidget* parent = nullptr);

private:
    void updateClientCount(int count);

    QLabel* clientCountValue_ = nullptr;
};

} // namespace charging::server
