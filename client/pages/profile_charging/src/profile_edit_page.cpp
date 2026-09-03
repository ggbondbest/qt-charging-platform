#include "charging/client/profile_charging/profile_edit_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/loading_overlay.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"
#include "charging/common/protocol/protocol.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace charging::client {

namespace {

// Mirrors the schema CHECK bound (trim(nickname) is 1..32 characters). The
// service repeats the check and the server stays authoritative.
bool isUsableNickname(const QString& nickname)
{
    const QString trimmed = nickname.trimmed();
    return !trimmed.isEmpty() && trimmed.length() <= 32;
}

} // namespace

ProfileEditPage::ProfileEditPage(WalletService* service, QWidget* parent)
    : QWidget(parent), service_(service)
{
    buildUi();

    connect(service_, &WalletService::profileLoaded, this, &ProfileEditPage::onProfileLoaded);
    connect(service_, &WalletService::operationFailed, this, &ProfileEditPage::onOperationFailed);
}

void ProfileEditPage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(14);

    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("编辑资料"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    auto* backButton = new ActionButton(ActionButton::Variant::Ghost, tr("‹ 返回"), this);
    connect(backButton, &ActionButton::clicked, this, &ProfileEditPage::backRequested);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    headerRow->addWidget(backButton);
    rootLayout->addLayout(headerRow);

    // Identity card: avatar placeholder, nickname, phone, disabled upload.
    auto* identityCard = new Card(this);
    auto* identityLayout = identityCard->bodyLayout();
    identityLayout->setAlignment(Qt::AlignHCenter);

    avatarLabel_ = new QLabel(QStringLiteral("用"), identityCard);
    avatarLabel_->setObjectName(QStringLiteral("uiAvatar"));
    avatarLabel_->setFixedSize(64, 64);
    avatarLabel_->setAlignment(Qt::AlignCenter);
    identityLayout->addWidget(avatarLabel_, 0, Qt::AlignHCenter);

    identityNameLabel_ = new QLabel(tr("未登录"), identityCard);
    identityNameLabel_->setProperty("role", QStringLiteral("subtitle"));
    identityLayout->addWidget(identityNameLabel_, 0, Qt::AlignHCenter);

    identityPhoneLabel_ = new QLabel(QStringLiteral("--"), identityCard);
    identityPhoneLabel_->setProperty("role", QStringLiteral("caption"));
    identityLayout->addWidget(identityPhoneLabel_, 0, Qt::AlignHCenter);

    avatarButton_ = new ActionButton(ActionButton::Variant::Ghost, tr("更换头像"), identityCard);
    avatarButton_->setEnabled(false); // TODO(contract): avatar upload protocol undefined.
    avatarButton_->setToolTip(tr("头像上传接口待与组长确认"));
    identityLayout->addWidget(avatarButton_, 0, Qt::AlignHCenter);

    auto* avatarCaption = new QLabel(tr("头像上传能力待与组长确认"), identityCard);
    avatarCaption->setProperty("role", QStringLiteral("caption"));
    avatarCaption->setAlignment(Qt::AlignCenter);
    identityLayout->addWidget(avatarCaption, 0, Qt::AlignHCenter);
    rootLayout->addWidget(identityCard);

    // Account card: editable nickname row plus read-only rows.
    auto* accountCard = new Card(this);
    auto* accountLayout = accountCard->bodyLayout();

    auto* nicknameRow = new QWidget(accountCard);
    auto* nicknameRowLayout = new QHBoxLayout(nicknameRow);
    nicknameRowLayout->setContentsMargins(0, 0, 0, 0);
    auto* nicknameCaption = new QLabel(tr("昵称"), nicknameRow);
    nicknameCaption->setProperty("role", QStringLiteral("secondary"));
    nicknameValueLabel_ = new QLabel(QStringLiteral("--"), nicknameRow);
    nicknameValueLabel_->setProperty("role", QStringLiteral("infoValue"));
    editButton_ = new ActionButton(ActionButton::Variant::Ghost, tr("编辑"), nicknameRow);
    connect(editButton_, &ActionButton::clicked, this, &ProfileEditPage::startEditing);
    nicknameRowLayout->addWidget(nicknameCaption);
    nicknameRowLayout->addStretch();
    nicknameRowLayout->addWidget(nicknameValueLabel_);
    nicknameRowLayout->addWidget(editButton_);
    accountLayout->addWidget(nicknameRow);

    auto* nicknameEditRow = new QWidget(accountCard);
    auto* nicknameEditRowLayout = new QHBoxLayout(nicknameEditRow);
    nicknameEditRowLayout->setContentsMargins(0, 0, 0, 0);
    nicknameEdit_ = new QLineEdit(nicknameEditRow);
    nicknameEdit_->setObjectName(QStringLiteral("uiNicknameEdit"));
    nicknameEdit_->setMaxLength(32);
    nicknameEdit_->setPlaceholderText(tr("1–32 个字符"));
    saveButton_ = new ActionButton(ActionButton::Variant::Primary, tr("保存"), nicknameEditRow);
    connect(saveButton_, &ActionButton::clicked, this, &ProfileEditPage::saveEditing);
    cancelButton_ = new ActionButton(ActionButton::Variant::Ghost, tr("取消"), nicknameEditRow);
    connect(cancelButton_, &ActionButton::clicked, this, &ProfileEditPage::cancelEditing);
    nicknameEditRowLayout->addWidget(nicknameEdit_, 1);
    nicknameEditRowLayout->addWidget(saveButton_);
    nicknameEditRowLayout->addWidget(cancelButton_);
    accountLayout->addWidget(nicknameEditRow);

    nicknameHintLabel_ = new QLabel(tr("昵称需为 1–32 个字符"), accountCard);
    nicknameHintLabel_->setProperty("role", QStringLiteral("hintWarn"));
    nicknameHintLabel_->setVisible(false);
    accountLayout->addWidget(nicknameHintLabel_);

    nicknameEditRow->setVisible(false);

    auto* phoneRow = new QWidget(accountCard);
    auto* phoneRowLayout = new QHBoxLayout(phoneRow);
    phoneRowLayout->setContentsMargins(0, 0, 0, 0);
    auto* phoneCaption = new QLabel(tr("手机号"), phoneRow);
    phoneCaption->setProperty("role", QStringLiteral("secondary"));
    phoneValueLabel_ = new QLabel(QStringLiteral("--"), phoneRow);
    phoneValueLabel_->setProperty("role", QStringLiteral("infoValue"));
    phoneRowLayout->addWidget(phoneCaption);
    phoneRowLayout->addStretch();
    phoneRowLayout->addWidget(phoneValueLabel_);
    accountLayout->addWidget(phoneRow);

    auto* createdRow = new QWidget(accountCard);
    auto* createdRowLayout = new QHBoxLayout(createdRow);
    createdRowLayout->setContentsMargins(0, 0, 0, 0);
    auto* createdCaption = new QLabel(tr("注册时间"), createdRow);
    createdCaption->setProperty("role", QStringLiteral("secondary"));
    createdValueLabel_ = new QLabel(QStringLiteral("--"), createdRow);
    createdValueLabel_->setProperty("role", QStringLiteral("infoValue"));
    createdRowLayout->addWidget(createdCaption);
    createdRowLayout->addStretch();
    createdRowLayout->addWidget(createdValueLabel_);
    accountLayout->addWidget(createdRow);
    rootLayout->addWidget(accountCard);

    profileNotice_ = new NoticePanel(QStringLiteral("⚠"), tr("资料加载失败"), QString(),
                                     tr("重试"), this);
    profileNotice_->setVisible(false);
    connect(profileNotice_, &NoticePanel::actionTriggered, this, &ProfileEditPage::refresh);
    rootLayout->addWidget(profileNotice_);

    rootLayout->addStretch();

    overlay_ = new LoadingOverlay(this);
    setEditingVisible(false);
}

void ProfileEditPage::refresh()
{
    profileNotice_->setVisible(false);
    service_->fetchProfile();
}

void ProfileEditPage::startEditing()
{
    if (!hasUser_) {
        return;
    }
    nicknameEdit_->setText(user_.nickname);
    nicknameHintLabel_->setVisible(false);
    setEditingVisible(true);
    nicknameEdit_->setFocus();
    nicknameEdit_->selectAll();
}

void ProfileEditPage::cancelEditing()
{
    nicknameHintLabel_->setVisible(false);
    setEditingVisible(false);
}

void ProfileEditPage::saveEditing()
{
    if (saveInFlight_ || service_->isUpdatingNickname()) {
        return; // One save at a time; the reply will release the UI.
    }
    const QString nickname = nicknameEdit_->text().trimmed();
    if (!isUsableNickname(nickname)) {
        nicknameHintLabel_->setVisible(true); // Inline guard; the check above.
        return;
    }
    nicknameHintLabel_->setVisible(false);
    saveInFlight_ = true;
    beginBusy();
    editButton_->setEnabled(false);
    saveButton_->setEnabled(false);
    cancelButton_->setEnabled(false);
    nicknameEdit_->setEnabled(false);
    service_->updateNickname(nickname);
}

void ProfileEditPage::setEditingVisible(bool editing)
{
    // The value label and the edit field live in their own rows inside the
    // account card; swapping which row is visible switches the page between
    // display and edit mode.
    nicknameValueLabel_->parentWidget()->setVisible(!editing);
    nicknameEdit_->parentWidget()->setVisible(editing);
}

void ProfileEditPage::renderUser()
{
    const QString nickname = user_.nickname.isEmpty() ? tr("未设置") : user_.nickname;
    avatarLabel_->setText(nickname.isEmpty() ? QStringLiteral("用") : QString(nickname.at(0)));
    identityNameLabel_->setText(nickname);
    identityPhoneLabel_->setText(user_.phone);
    nicknameValueLabel_->setText(nickname);
    phoneValueLabel_->setText(user_.phone);
    createdValueLabel_->setText(user_.createdAtUtc.isValid()
                                    ? user_.createdAtUtc.toString(QStringLiteral("yyyy-MM-dd"))
                                    : QStringLiteral("--"));
}

void ProfileEditPage::onProfileLoaded(const charging::model::User& user)
{
    user_ = user;
    hasUser_ = true;
    profileNotice_->setVisible(false);
    renderUser();

    if (saveInFlight_) {
        saveInFlight_ = false;
        endBusy();
        editButton_->setEnabled(true);
        saveButton_->setEnabled(true);
        cancelButton_->setEnabled(true);
        nicknameEdit_->setEnabled(true);
        setEditingVisible(false);
        Toast::show(this, tr("昵称已更新"), StatusTag::Tone::Success);
    }
}

void ProfileEditPage::onOperationFailed(const QString& type,
                                    const charging::protocol::ProtocolError& error)
{
    const QString updateType =
        QString::fromLatin1(charging::protocol::request_type::kUpdateUserInfo);
    if (type == updateType) {
        if (saveInFlight_) {
            saveInFlight_ = false;
            endBusy();
            editButton_->setEnabled(true);
            saveButton_->setEnabled(true);
            cancelButton_->setEnabled(true);
            nicknameEdit_->setEnabled(true);
        }
        Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);
        return;
    }

    const QString getType =
        QString::fromLatin1(charging::protocol::request_type::kGetUserInfo);
    if (type == getType && !hasUser_) {
        profileNotice_->setContent(QStringLiteral("⚠"), tr("资料加载失败"),
                                   displayMessageForError(error), tr("重试"));
        profileNotice_->setVisible(true);
    }
    // Other profile traffic (e.g. a wallet recharge elsewhere) updates the
    // rendered user through profileLoaded; failures there stay toast-only.
}

void ProfileEditPage::beginBusy()
{
    ++busyCount_;
    if (busyCount_ == 1) {
        overlay_->showFor();
    }
}

void ProfileEditPage::endBusy()
{
    busyCount_ = busyCount_ > 0 ? busyCount_ - 1 : 0;
    if (busyCount_ == 0) {
        overlay_->hideFor();
    }
}

} // namespace charging::client
