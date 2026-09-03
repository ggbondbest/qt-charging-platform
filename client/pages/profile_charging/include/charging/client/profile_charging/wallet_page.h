#pragma once

#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/models.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace charging::client {

class ActionButton;
class Card;
class LoadingOverlay;
class NoticePanel;

// Wallet page: current balance, recharge entry and recharge record list.
// Pure presentation: every number shown here comes from WalletService.
class WalletPage final : public QWidget
{
    Q_OBJECT

public:
    explicit WalletPage(WalletService* service, QWidget* parent = nullptr);

    // Re-fetch profile and first records page; called on entry and after a
    // recharge so the page never keeps stale balances.
    void refresh();

signals:
    void rechargeRequested();
    void ordersRequested();

private slots:
    void onProfileLoaded(const charging::model::User& user);
    void onRecordsLoaded(const QVector<charging::model::RechargeRecord>& records, int total,
                         bool hasMore);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    QWidget* buildRecordRow(const charging::model::RechargeRecord& record);
    void clearRecordRows();
    void showRecordsNotice(const QString& glyph, const QString& title, const QString& description,
                           const QString& actionText);
    void hideRecordsNotice();
    void beginBusy();
    void endBusy();

    WalletService* service_ = nullptr;

    QLabel* nicknameLabel_ = nullptr;
    QLabel* phoneLabel_ = nullptr;
    QLabel* balanceValueLabel_ = nullptr;
    Card* balanceCard_ = nullptr;
    ActionButton* rechargeButton_ = nullptr;
    NoticePanel* profileNotice_ = nullptr;

    QScrollArea* recordsScroll_ = nullptr;
    QVBoxLayout* recordsListLayout_ = nullptr;
    ActionButton* loadMoreButton_ = nullptr;
    NoticePanel* recordsNotice_ = nullptr;

    LoadingOverlay* overlay_ = nullptr;
    int busyCount_ = 0;

    QVector<charging::model::RechargeRecord> shownRecords_;
    int currentPage_ = 0;    // Last fully loaded page.
    int loadingPage_ = 0;    // Page requested by the currently in-flight call.
    bool hasMoreRecords_ = false;
};

} // namespace charging::client
