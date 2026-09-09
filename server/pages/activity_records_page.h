#pragma once

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

// Selects the read-only admin data source shown by this shared page.
enum class ActivityRecordsMode {
    Recharge,
    OperationLog,
};

class ActivityRecordsPage final : public QWidget
{
public:
    explicit ActivityRecordsPage(ActivityRecordsMode mode, QWidget* parent = nullptr);
    void setAdminGateway(class AdminRequestGateway* gateway);
    void refreshData() { if (realMode_) requestList(); }

private:
    struct Record {
        QString id;
        QString occurredAt;
        QString subject;
        QString category;
        QString amountOrTarget;
        QString status;
        QString details;
        QString serverId{};
    };

    void createMockRecords();
    void applyFilters();
    void resetFilters();
    void rebuildTable();
    void showDetails(int recordIndex, bool requestDetails = true);
    void showPreviousPage();
    void showNextPage();
    void manualRefresh();
    bool matchesFilters(const Record& record) const;
    void setFeedback(const QString& text, bool isError = false);
    void requestList();
    void handleListResponse(const QJsonObject& response);
    void handleSummaryResponse(const QJsonObject& response);
    void handleDetailResponse(const QJsonObject& response);
    void requestMetadata();
    void handleActionsResponse(const QJsonObject& response);

    ActivityRecordsMode mode_;
    QVector<Record> records_;
    QVector<int> filteredRecordIndexes_;
    int currentPage_ = 0;
    int selectedRecordIndex_ = -1;
    int totalRecords_ = 0;
    bool realMode_ = false;
    bool hasRealSnapshot_ = false;
    QString listRequestId_;
    QString summaryRequestId_;
    QString detailRequestId_;
    QString detailExpectedServerId_;
    QString actionsRequestId_;
    class AdminOptionLoader* adminOptions_ = nullptr;
    class AdminRequestGateway* gateway_ = nullptr;

    QLineEdit* keywordLineEdit_ = nullptr;
    QComboBox* categoryComboBox_ = nullptr;
    QComboBox* adminComboBox_ = nullptr;
    QComboBox* statusComboBox_ = nullptr;
    QComboBox* dateRangeComboBox_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QLabel* tableTitleLabel_ = nullptr;
    QLabel* feedbackLabel_ = nullptr;
    QLabel* paginationLabel_ = nullptr;
    ManagementStatePanel* statePanel_ = nullptr;
    QLabel* detailTitleLabel_ = nullptr;
    QLabel* detailMetaLabel_ = nullptr;
    QLabel* detailContentLabel_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* nextPageButton_ = nullptr;
};

} // namespace charging::server
