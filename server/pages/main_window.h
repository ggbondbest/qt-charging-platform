#pragma once

#include <QMainWindow>
#include <QString>
#include <QTimer>

class QLabel;
class QMenu;
class QFrame;
class QResizeEvent;
class QStackedWidget;
class QButtonGroup;
class QVBoxLayout;
class QEvent;

namespace charging::server {

class AdminLoginPage;
class AdminRequestGateway;
class ChargerManagementPage;
class OrderManagementPage;
class ServerRuntime;
class StationManagementPage;
class UserManagementPage;
class ActivityRecordsPage;
class WorkflowManagementPage;
class IntelligenceCenterPage;
class DashboardServiceManager;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ServerRuntime* server, QWidget* parent = nullptr);
    AdminRequestGateway* adminGateway() const { return adminGateway_; }
    void setDashboardServiceManager(DashboardServiceManager* manager);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private slots:
    void handleLoginSubmitted(const QString& username, const QString& password);

private:
    QWidget* createManagementPage();
    void showManagementShell();
    void showLoginPage();
    void updateSidebarWidth();
    void refreshActivePage();
    void updateManagementChrome();
    void setDashboardFullScreen(bool enabled);
    void exitDashboardFullScreen();

    ServerRuntime* server_ = nullptr;
    AdminRequestGateway* adminGateway_ = nullptr;
    QString loginRequestId_;
    QStackedWidget* rootStackedWidget_ = nullptr;
    QStackedWidget* pageStackedWidget_ = nullptr;
    AdminLoginPage* loginPage_ = nullptr;
    ChargerManagementPage* chargerManagementPage_ = nullptr;
    StationManagementPage* stationManagementPage_ = nullptr;
    UserManagementPage* userManagementPage_ = nullptr;
    OrderManagementPage* orderManagementPage_ = nullptr;
    ActivityRecordsPage* rechargeRecordsPage_ = nullptr;
    ActivityRecordsPage* operationLogPage_ = nullptr;
    WorkflowManagementPage* workflowPage_ = nullptr;
    IntelligenceCenterPage* intelligenceCenterPage_ = nullptr;
    QTimer autoRefreshTimer_;
    QFrame* sidebar_ = nullptr;
    QWidget* topBar_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QButtonGroup* navigationGroup_ = nullptr;
    QByteArray previousWindowGeometry_;
    Qt::WindowStates previousWindowState_;
    bool dashboardFullScreen_ = false;
    QLabel* pageTitleLabel_ = nullptr;
    QMenu* sessionMenu_ = nullptr;
};

} // namespace charging::server
