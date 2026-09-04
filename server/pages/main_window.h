#pragma once

#include <QMainWindow>
#include <QString>

class QLabel;
class QFrame;
class QResizeEvent;
class QStackedWidget;

namespace charging::server {

class AdminLoginPage;
class ChargingServer;
class DashboardPage;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ChargingServer* server, QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void handleLoginSubmitted(const QString& username, const QString& password);

private:
    QWidget* createManagementPage();
    void showManagementShell();
    void showLoginPage();
    void updateClientCount(int count);
    void updateSidebarWidth();

    ChargingServer* server_ = nullptr;
    QStackedWidget* rootStackedWidget_ = nullptr;
    QStackedWidget* pageStackedWidget_ = nullptr;
    AdminLoginPage* loginPage_ = nullptr;
    DashboardPage* dashboardPage_ = nullptr;
    QFrame* sidebar_ = nullptr;
    QLabel* pageTitleLabel_ = nullptr;
    QLabel* pageSubtitleLabel_ = nullptr;
};

} // namespace charging::server
