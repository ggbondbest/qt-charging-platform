#pragma once

#include <QWidget>

class QLabel;

namespace charging::server {

class DashboardPage final : public QWidget
{
    Q_OBJECT

public:
    explicit DashboardPage(QWidget* parent = nullptr);

    void setClientCount(int count);

private:
    QLabel* clientCountValue_ = nullptr;
};

} // namespace charging::server
