#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace charging::server {

class StationManagementPage final : public QWidget
{
    Q_OBJECT

public:
    explicit StationManagementPage(QWidget* parent = nullptr);

private slots:
    void applyFilters();
    void resetFilters();
    void showAddStationDialog();
    void showEditStationDialog();
    void toggleSelectedStationStatus();
    void showPreviousPage();
    void showNextPage();

private:
    struct StationRecord {
        QString code;
        QString name;
        QString city;
        QString district;
        QString address;
        QString status;
        int chargerCount = 0;
        int fastChargerCount = 0;
        int slowChargerCount = 0;
        int todayOrders = 0;
        int utilizationPercent = 0;
        QString contactName;
        QString contactPhone;
    };

    void createMockRecords();
    void rebuildTable();
    void updateEmptyState();
    void showStationDetails(int recordIndex);
    void updateDetailActions();
    void showStationDialog(int recordIndex);
    void setFeedback(const QString& text);
    bool recordMatchesFilters(const StationRecord& record) const;

    QVector<StationRecord> records_;
    QVector<int> filteredRecordIndexes_;
    int selectedRecordIndex_ = -1;
    int currentPage_ = 0;

    QLineEdit* keywordLineEdit_ = nullptr;
    QComboBox* cityComboBox_ = nullptr;
    QComboBox* districtComboBox_ = nullptr;
    QComboBox* statusComboBox_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QLabel* tableTitleLabel_ = nullptr;
    QLabel* emptyStateLabel_ = nullptr;
    QLabel* feedbackLabel_ = nullptr;
    QLabel* paginationLabel_ = nullptr;
    QLabel* detailNameLabel_ = nullptr;
    QLabel* detailIdLabel_ = nullptr;
    QLabel* detailStatusLabel_ = nullptr;
    QLabel* detailAddressLabel_ = nullptr;
    QLabel* detailContactLabel_ = nullptr;
    QLabel* detailConfigurationLabel_ = nullptr;
    QLabel* detailRealtimeLabel_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* nextPageButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* toggleStatusButton_ = nullptr;
};

} // namespace charging::server
