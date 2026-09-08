#pragma once

#include "admin_mock_data.h"

#include <QString>
#include <QtGlobal>
#include <QVector>
#include <QWidget>
#include <QJsonObject>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace charging::server {

class ManagementStatePanel;

class OrderManagementPage final : public QWidget
{
    Q_OBJECT

public:
    explicit OrderManagementPage(QWidget* parent = nullptr);
    void setAdminGateway(class AdminRequestGateway* gateway);
    void refreshData() { if (realMode_) requestList(); }

    void showLatestOrders();

private slots:
    void applyFilters();
    void resetFilters();
    void refreshOrderList();
    void refreshSelectedOrder();
    void showPreviousPage();
    void showNextPage();

private:
    using OrderRecord = admin_mock::OrderRecord;

    void createMockRecords();
    void rebuildTable();
    void updateEmptyState();
    void showOrderDetails(int recordIndex, bool requestDetails = true);
    void setFeedback(const QString& text);
    bool recordMatchesFilters(const OrderRecord& record) const;
    void requestList();
    void requestFilterOptions();
    void handleListResponse(const QJsonObject& response);
    void handleSummaryResponse(const QJsonObject& response);
    void handleDetailResponse(const QJsonObject& response);
    void handleStationOptionsResponse(const QJsonObject& response);
    void handleChargerOptionsResponse(const QJsonObject& response);

    QVector<OrderRecord> records_;
    QVector<int> filteredRecordIndexes_;
    int selectedRecordIndex_ = -1;
    int currentPage_ = 0;
    int totalRecords_ = 0;
    bool realMode_ = false;
    bool hasRealSnapshot_ = false;
    QString listRequestId_;
    QString summaryRequestId_;
    QString detailRequestId_;
    QString detailExpectedServerId_;
    QString stationOptionsRequestId_;
    QString chargerOptionsRequestId_;
    class AdminRequestGateway* gateway_ = nullptr;

    QLineEdit* orderNumberLineEdit_ = nullptr;
    QLineEdit* userLineEdit_ = nullptr;
    QLineEdit* phoneLineEdit_ = nullptr;
    QComboBox* stationComboBox_ = nullptr;
    QComboBox* chargerComboBox_ = nullptr;
    QComboBox* statusComboBox_ = nullptr;
    QComboBox* dateRangeComboBox_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QLabel* tableTitleLabel_ = nullptr;
    ManagementStatePanel* statePanel_ = nullptr;
    QLabel* feedbackLabel_ = nullptr;
    QLabel* paginationLabel_ = nullptr;
    QLabel* detailOrderNumberLabel_ = nullptr;
    QLabel* detailStatusLabel_ = nullptr;
    QLabel* detailCreatedAtLabel_ = nullptr;
    QLabel* chargingInfoLabel_ = nullptr;
    QLabel* feeInfoLabel_ = nullptr;
    QLabel* paymentInfoLabel_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* nextPageButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
};

} // namespace charging::server
