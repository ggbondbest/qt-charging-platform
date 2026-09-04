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

class ChargerManagementPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ChargerManagementPage(QWidget* parent = nullptr);

private slots:
    void applyFilters();
    void resetFilters();
    void refreshSelectedStatus();
    void restartSelectedCharger();
    void clearSelectedAlert();
    void showPreviousPage();
    void showNextPage();

private:
    struct ChargerRecord {
        QString code;
        QString station;
        QString type;
        QString power;
        QString status;
        int todaySessions = 0;
        int totalSessions = 0;
        QString totalDuration;
        QString lastHeartbeat;
    };

    void createMockRecords();
    void rebuildTable();
    void updateEmptyState();
    void showChargerDetails(int recordIndex);
    void updateDetailActions();
    void setFeedback(const QString& text);
    bool recordMatchesFilters(const ChargerRecord& record) const;

    QVector<ChargerRecord> records_;
    QVector<int> filteredRecordIndexes_;
    int selectedRecordIndex_ = -1;
    int currentPage_ = 0;

    QLineEdit* keywordLineEdit_ = nullptr;
    QComboBox* stationComboBox_ = nullptr;
    QComboBox* statusComboBox_ = nullptr;
    QComboBox* typeComboBox_ = nullptr;
    QComboBox* powerComboBox_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QLabel* tableTitleLabel_ = nullptr;
    QLabel* emptyStateLabel_ = nullptr;
    QLabel* feedbackLabel_ = nullptr;
    QLabel* paginationLabel_ = nullptr;
    QLabel* detailCodeLabel_ = nullptr;
    QLabel* detailStationLabel_ = nullptr;
    QLabel* detailStatusLabel_ = nullptr;
    QLabel* detailBasicInfoLabel_ = nullptr;
    QLabel* detailRuntimeInfoLabel_ = nullptr;
    QLabel* alertCountLabel_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* nextPageButton_ = nullptr;
    QPushButton* restartButton_ = nullptr;
    QPushButton* refreshStatusButton_ = nullptr;
    QPushButton* clearAlertButton_ = nullptr;
};

} // namespace charging::server
