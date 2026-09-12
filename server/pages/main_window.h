#pragma once

#include <QMainWindow>
#include <QString>
#include <QTimer>

class QLabel;
class QFrame;
class QResizeEvent;
class QStackedWidget;

namespace charging::server {

class AdminLoginPage;
class AdminRequestGateway;
class ChargerManagementPage;
class OrderManagementPage;
class ServerRuntime;
class DashboardPage;
class StationManagementPage;
class UserManagementPage;
class ActivityRecordsPage;
class WorkflowManagementPage;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ServerRuntime* server, QWidget* parent = nullptr);
    AdminRequestGateway* adminGateway() const { return adminGateway_; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void handleLoginSubmitted(const QString& username, const QString& password);

private:
    QWidget* createManagementPage();
    void showManagementShell();
    void showLoginPage();
    void updateSidebarWidth();
    void refreshActivePage();

    ServerRuntime* server_ = nullptr;
    AdminRequestGateway* adminGateway_ = nullptr;
    QString loginRequestId_;
    QStackedWidget* rootStackedWidget_ = nullptr;
    QStackedWidget* pageStackedWidget_ = nullptr;
    AdminLoginPage* loginPage_ = nullptr;
    DashboardPage* dashboardPage_ = nullptr;
    ChargerManagementPage* chargerManagementPage_ = nullptr;
    StationManagementPage* stationManagementPage_ = nullptr;
    UserManagementPage* userManagementPage_ = nullptr;
    OrderManagementPage* orderManagementPage_ = nullptr;
    ActivityRecordsPage* rechargeRecordsPage_ = nullptr;
    ActivityRecordsPage* operationLogPage_ = nullptr;
    WorkflowManagementPage* workflowPage_ = nullptr;
    QTimer autoRefreshTimer_;
    QFrame* sidebar_ = nullptr;
    QLabel* pageTitleLabel_ = nullptr;
    QLabel* pageSubtitleLabel_ = nullptr;
};

} // namespace charging::server
