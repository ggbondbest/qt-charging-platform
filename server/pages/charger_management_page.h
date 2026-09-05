#pragma once

#include "admin_mock_data.h"

#include <QString>
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

class ChargerManagementPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ChargerManagementPage(QWidget* parent = nullptr);

    void setAdminGateway(class AdminRequestGateway* gateway);

    void showExceptionRecords();
    void showExceptionRecord(const QString& chargerCode);

private slots:
    void applyFilters();
    void resetFilters();
    void refreshSelectedStatus();
    void restartSelectedCharger();
    void clearSelectedAlert();
    void showAddChargerDialog();
    void showEditChargerDialog();
    void showPreviousPage();
    void showNextPage();

private:
    using ChargerRecord = admin_mock::ChargerRecord;

    void createMockRecords();
    void rebuildTable();
    void updateEmptyState();
    void showChargerDetails(int recordIndex);
    void updateDetailActions();
    void showChargerDialog(int recordIndex);
    void setFeedback(const QString& text);
    bool recordMatchesFilters(const ChargerRecord& record) const;
    void requestList();
    void handleListResponse(const QJsonObject& response);
    void handleWriteResponse(const QJsonObject& response);
    QString statusCode(const QString& display) const;

    QVector<ChargerRecord> records_;
    QVector<int> filteredRecordIndexes_;
    int selectedRecordIndex_ = -1;
    int currentPage_ = 0;
    int totalRecords_ = 0;
    bool realMode_ = false;
    QString listRequestId_;
    QString writeRequestId_;
    QString detailRequestId_;
    class AdminRequestGateway* gateway_ = nullptr;

    QLineEdit* keywordLineEdit_ = nullptr;
    QComboBox* stationComboBox_ = nullptr;
    QComboBox* statusComboBox_ = nullptr;
    QComboBox* typeComboBox_ = nullptr;
    QComboBox* powerComboBox_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QLabel* tableTitleLabel_ = nullptr;
    ManagementStatePanel* statePanel_ = nullptr;
    QLabel* feedbackLabel_ = nullptr;
    QLabel* paginationLabel_ = nullptr;
    QLabel* detailCodeLabel_ = nullptr;
    QLabel* detailStationLabel_ = nullptr;
    QLabel* detailStatusLabel_ = nullptr;
    QLabel* detailBasicInfoLabel_ = nullptr;
    QLabel* detailRuntimeInfoLabel_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* nextPageButton_ = nullptr;
    QPushButton* restartButton_ = nullptr;
    QPushButton* refreshStatusButton_ = nullptr;
    QPushButton* clearAlertButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
};

} // namespace charging::server
