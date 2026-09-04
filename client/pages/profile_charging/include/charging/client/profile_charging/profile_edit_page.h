#pragma once

#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;
class QLineEdit;

namespace charging::client {

class ActionButton;
class Card;
class LoadingOverlay;
class NoticePanel;

// Profile edit page (编辑资料), opened from the ProfilePage hub: identity
// block with an avatar placeholder and the editable nickname, plus read-only
// account rows. The nickname is persisted by WalletService::updateNickname
// and the page re-renders from the user the server returns; the client-side
// length check is a guard, not the rule. Avatar upload has no protocol yet,
// so its button stays disabled.
class ProfileEditPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ProfileEditPage(WalletService* service, QWidget* parent = nullptr);

    // Re-fetch the profile on entry so the page never shows stale data.
    void refresh();
    // 整合壳层（HomeShell）内使用：隐藏页内返回按钮（全局顶部导航负责返回）。
    void setEmbedded(bool embedded);

signals:
    void backRequested();

private slots:
    void onProfileLoaded(const charging::model::User& user);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    void startEditing();
    void cancelEditing();
    void saveEditing();
    void setEditingVisible(bool editing);
    void renderUser();
    void beginBusy();
    void endBusy();

    WalletService* service_ = nullptr;

    QLabel* avatarLabel_ = nullptr;
    QLabel* identityNameLabel_ = nullptr;
    QLabel* identityPhoneLabel_ = nullptr;
    ActionButton* backButton_ = nullptr; // 页内返回（嵌入壳层时隐藏）
    ActionButton* avatarButton_ = nullptr;

    QLabel* nicknameValueLabel_ = nullptr;
    QLineEdit* nicknameEdit_ = nullptr;
    ActionButton* editButton_ = nullptr;
    ActionButton* saveButton_ = nullptr;
    ActionButton* cancelButton_ = nullptr;
    QLabel* nicknameHintLabel_ = nullptr;

    QLabel* phoneValueLabel_ = nullptr;
    QLabel* createdValueLabel_ = nullptr;
    NoticePanel* profileNotice_ = nullptr;

    LoadingOverlay* overlay_ = nullptr;
    int busyCount_ = 0;

    charging::model::User user_;
    bool hasUser_ = false;
    bool saveInFlight_ = false; // A nickname save is waiting for the reply.
};

} // namespace charging::client
