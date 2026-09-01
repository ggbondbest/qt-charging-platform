#pragma once

#include <QMainWindow>

namespace charging::client {

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
};

} // namespace charging::client
