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

class OrderManagementPage final : public QWidget
{
    Q_OBJECT

public:
    explicit OrderManagementPage(QWidget* parent = nullptr);

private slots:
    void applyFilters();
    void resetFilters();
    void refreshSelectedOrder();
    void showPreviousPage();
    void showNextPage();

private:
    struct OrderRecord {
        QString orderNo;
        QString userName;
        QString phone;
        QString station;
        QString charger;
        QString chargerType;
        QString status;
        QString startAt;
        QString duration;
        double energyKwh = 0.0;
        double chargeFee = 0.0;
        double serviceFee = 0.0;
        double discountFee = 0.0;
        QString paymentMethod;
        QString paymentStatus;
    };

    void createMockRecords();
    void rebuildTable();
    void updateEmptyState();
    void showOrderDetails(int recordIndex);
    void setFeedback(const QString& text);
    bool recordMatchesFilters(const OrderRecord& record) const;

    QVector<OrderRecord> records_;
    QVector<int> filteredRecordIndexes_;
    int selectedRecordIndex_ = -1;
    int currentPage_ = 0;

    QLineEdit* orderNumberLineEdit_ = nullptr;
    QLineEdit* userLineEdit_ = nullptr;
    QLineEdit* phoneLineEdit_ = nullptr;
    QComboBox* stationComboBox_ = nullptr;
    QComboBox* chargerComboBox_ = nullptr;
    QComboBox* statusComboBox_ = nullptr;
    QComboBox* dateRangeComboBox_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QLabel* tableTitleLabel_ = nullptr;
    QLabel* emptyStateLabel_ = nullptr;
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
