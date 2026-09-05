#pragma once

#include <QJsonObject>
#include <QString>
#include <QWidget>

class QLabel;
class QTableWidget;
class QPushButton;

namespace charging::server {

class RevenueTrendWidget;
class DeviceStatusWidget;

class DashboardPage final : public QWidget
{
    Q_OBJECT

public:
    explicit DashboardPage(QWidget* parent = nullptr);

    void setAdminGateway(class AdminRequestGateway* gateway);
    void refresh(int days = 7);
    void setClientCount(int count);

signals:
    void exceptionListRequested();
    void latestOrdersRequested();

private:
    void handleDashboardResponse(const QJsonObject& response);
    class AdminRequestGateway* gateway_ = nullptr;
    QString requestId_;
    QLabel* clientCountValue_ = nullptr;
    QLabel* todayRevenueValue_ = nullptr;
    QLabel* monthRevenueValue_ = nullptr;
    QLabel* onlineChargersValue_ = nullptr;
    QLabel* onlineChargersHint_ = nullptr;
    QLabel* totalChargersLabel_ = nullptr;
    QLabel* refreshedAtLabel_ = nullptr;
    QLabel* exceptionCountBadge_ = nullptr;
    QTableWidget* exceptionTable_ = nullptr;
    QTableWidget* latestOrdersTable_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    RevenueTrendWidget* trendWidget_ = nullptr;
    DeviceStatusWidget* deviceStatusWidget_ = nullptr;
    QLabel* onlineLegendValue_ = nullptr;
    QLabel* offlineLegendValue_ = nullptr;
    QLabel* faultLegendValue_ = nullptr;
    int requestedDays_ = 7;
};

} // namespace charging::server
