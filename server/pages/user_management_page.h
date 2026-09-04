#pragma once

#include <QString>
#include <QtGlobal>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace charging::server {

class UserManagementPage final : public QWidget
{
    Q_OBJECT

public:
    explicit UserManagementPage(QWidget* parent = nullptr);

private slots:
    void applyFilters();
    void resetFilters();
    void toggleSelectedUserStatus();
    void toggleSelectedRiskFocus();
    void showPreviousPage();
    void showNextPage();

private:
    struct UserRecord {
        QString id;
        QString nickname;
        QString phone;
        qint64 balanceCents = 0;
        QString status;
        QString registeredAt;
        QString lastChargeAt;
        int totalOrders = 0;
        bool isRiskFocused = false;
        bool isRecentRegistration = false;
    };

    void createMockRecords();
    void rebuildTable();
    void updateEmptyState();
    void showUserDetails(int recordIndex);
    void updateDetailActions();
    void setFeedback(const QString& text, bool isError = false);
    bool recordMatchesFilters(const UserRecord& record) const;

    QVector<UserRecord> records_;
    QVector<int> filteredRecordIndexes_;
    int selectedRecordIndex_ = -1;
    int currentPage_ = 0;

    QLineEdit* keywordLineEdit_ = nullptr;
    QComboBox* statusComboBox_ = nullptr;
    QComboBox* registrationComboBox_ = nullptr;
    QLineEdit* minimumBalanceLineEdit_ = nullptr;
    QLineEdit* maximumBalanceLineEdit_ = nullptr;
    QTableWidget* tableWidget_ = nullptr;
    QLabel* tableTitleLabel_ = nullptr;
    QLabel* emptyStateLabel_ = nullptr;
    QLabel* feedbackLabel_ = nullptr;
    QLabel* paginationLabel_ = nullptr;
    QLabel* avatarLabel_ = nullptr;
    QLabel* detailNameLabel_ = nullptr;
    QLabel* detailIdLabel_ = nullptr;
    QLabel* detailPhoneLabel_ = nullptr;
    QLabel* detailAccountLabel_ = nullptr;
    QLabel* riskTagLabel_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* nextPageButton_ = nullptr;
    QPushButton* freezeButton_ = nullptr;
    QPushButton* riskButton_ = nullptr;
};

} // namespace charging::server
