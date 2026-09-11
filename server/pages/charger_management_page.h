#pragma once

#include "admin_mock_data.h"

#include <QString>
#include <QVector>
#include <QWidget>
#include <QJsonObject>
#include <QHash>

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
    void refreshData() { if (realMode_) { requestStationOptions(true); requestList(); } }

    void showExceptionRecords();
    void showExceptionRecord(const QString& chargerCode);
    void showStationRecords(const QString& stationId);

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
    friend class AdminChargerExtensionPagesTest;
    using ChargerRecord = admin_mock::ChargerRecord;

    void createMockRecords();
    void rebuildTable();
    void updateEmptyState();
    void showChargerDetails(int recordIndex, bool requestDetails = true);
    void updateDetailActions();
    void showChargerDialog(int recordIndex);
    void setFeedback(const QString& text);
    bool recordMatchesFilters(const ChargerRecord& record) const;
    void requestList();
    void requestStationOptions(bool onlyRetryFailed = false);
    void requestStationById(const QString& stationId);
    void handleListResponse(const QJsonObject& response);
    void handleStationLookupResponse(const QJsonObject& response);
    void handleSummaryResponse(const QJsonObject& response);
    void handleDetailResponse(const QJsonObject& response);
    void handleWriteResponse(const QJsonObject& response);
    void handleRuntimeResponse(const QJsonObject& response);
    void renderRuntime();
    void submitWrite(const QString& action, const QJsonObject& parameters);
    void retryPendingWrite();
    void sendPendingWrite();
    void showWriteStatus();
    QString statusCode(const QString& display) const;
    QString selectedChargerTypeCode() const;
    int selectedPowerWatts() const;

    QVector<ChargerRecord> records_;
    QVector<int> filteredRecordIndexes_;
    int selectedRecordIndex_ = -1;
    int currentPage_ = 0;
    int totalRecords_ = 0;
    bool realMode_ = false;
    bool hasRealSnapshot_ = false;
    QString listRequestId_;
    QString summaryRequestId_;
    QString writeRequestId_;
    QString pendingWriteAction_;
    QJsonObject pendingWriteParameters_;
    QString pendingWriteAdminId_;
    bool writeInFlight_ = false;
    bool writeOutcomeUnknown_ = false;
    QString detailRequestId_;
    QString detailExpectedServerId_;
    QString runtimeRequestId_;
    QString runtimeExpectedServerId_;
    QHash<QString, QJsonObject> activeExceptions_;
    QHash<QString, QJsonObject> runtimeSnapshots_;
    // This is only used while station options are loading.  The combo box is
    // the sole source of the actual list filter, so it cannot be overridden.
    QString pendingStationFilterId_;
    class AdminOptionLoader* stationOptions_ = nullptr;
    QString stationLookupRequestId_;
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
    QLabel* detailLiveChargeLabel_ = nullptr;
    QLabel* detailRuntimeInfoLabel_ = nullptr;
    QLabel* detailRecoveryLabel_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* nextPageButton_ = nullptr;
    QPushButton* restartButton_ = nullptr;
    QPushButton* refreshStatusButton_ = nullptr;
    QPushButton* clearAlertButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* retryWriteButton_ = nullptr;
    QLabel* writeStatusLabel_ = nullptr;
};

} // namespace charging::server
