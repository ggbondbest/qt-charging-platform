#include "charging/client/profile_charging/profile_page.h"

#include "charging/client/profile_charging/avatar_library.h"
#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/common/protocol/protocol.h"

#include <QColor>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace charging::client {

ProfilePage::ProfilePage(WalletService* walletService, OrderService* orderService,
                         QWidget* parent)
    : QWidget(parent), walletService_(walletService), orderService_(orderService)
{
    buildUi();

    connect(walletService_, &WalletService::profileLoaded, this, &ProfilePage::onProfileLoaded);
    connect(walletService_, &WalletService::operationFailed, this, &ProfilePage::onOperationFailed);
    connect(orderService_, &OrderService::statusCountsUpdated, this, &ProfilePage::onStatusCounts);
}

void ProfilePage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ---------- 渐变头图：头像/昵称/手机号，整块点击进编辑资料 ----------
    auto* heroButton = new QPushButton(this);
    heroButton->setObjectName(QStringLiteral("uiProfileHeroButton"));
    heroButton->setCursor(Qt::PointingHandCursor);
    heroButton->setMinimumHeight(150);
    connect(heroButton, &QPushButton::clicked, this, &ProfilePage::profileEditRequested);

    auto* heroLayout = new QHBoxLayout(heroButton);
    heroLayout->setContentsMargins(22, 28, 20, 26);
    heroLayout->setSpacing(14);

    avatarLabel_ = new QLabel(QStringLiteral("用"), heroButton);
    avatarLabel_->setObjectName(QStringLiteral("uiAvatarHub"));
    avatarLabel_->setFixedSize(64, 64);
    avatarLabel_->setAlignment(Qt::AlignCenter);
    heroLayout->addWidget(avatarLabel_);

    auto* identityText = new QVBoxLayout();
    identityText->setSpacing(4);
    identityNameLabel_ = new QLabel(tr("未登录"), heroButton);
    // objectName 沿用壳层测试锚点（QSS 在头图内覆写为白色）。
    identityNameLabel_->setObjectName(QStringLiteral("nicknameLabel"));
    identityPhoneLabel_ = new QLabel(QStringLiteral("--"), heroButton);
    identityPhoneLabel_->setProperty("role", QStringLiteral("heroPhone"));
    identityText->addWidget(identityNameLabel_);
    identityText->addWidget(identityPhoneLabel_);
    heroLayout->addLayout(identityText);
    heroLayout->addStretch();

    auto* editHint = new QLabel(tr("编辑资料 ›"), heroButton);
    editHint->setProperty("role", QStringLiteral("heroHint"));
    heroLayout->addWidget(editHint);
    rootLayout->addWidget(heroButton);

    // ---------- 钱包卡：余额/充值/充值记录三列，紧贴头图下沿 ----------
    auto* walletWrap = new QWidget(this);
    auto* walletWrapLayout = new QVBoxLayout(walletWrap);
    walletWrapLayout->setContentsMargins(16, 12, 16, 0);
    walletWrapLayout->setSpacing(0);

    auto* walletCard = new QWidget(walletWrap);
    walletCard->setObjectName(QStringLiteral("uiWalletCard"));
    walletCard->setAttribute(Qt::WA_StyledBackground, true);
    // 浮卡投影：轻、低透明度，只强调层级不抢焦点。
    auto* walletShadow = new QGraphicsDropShadowEffect(walletCard);
    walletShadow->setBlurRadius(24);
    walletShadow->setOffset(0, 6);
    walletShadow->setColor(QColor(31, 41, 55, 22));
    walletCard->setGraphicsEffect(walletShadow);

    auto* walletLayout = new QHBoxLayout(walletCard);
    walletLayout->setContentsMargins(8, 14, 8, 14);
    walletLayout->setSpacing(0);

    // 余额列：值为服务端权威余额，点击进钱包页。
    auto* balanceButton = new QPushButton(walletCard);
    balanceButton->setProperty("role", QStringLiteral("walletStat"));
    balanceButton->setCursor(Qt::PointingHandCursor);
    connect(balanceButton, &QPushButton::clicked, this, &ProfilePage::walletRequested);
    auto* balanceLayout = new QVBoxLayout(balanceButton);
    balanceLayout->setSpacing(4);
    balanceValueLabel_ = new QLabel(QStringLiteral("¥ --"), balanceButton);
    balanceValueLabel_->setObjectName(QStringLiteral("balanceLabel"));
    balanceValueLabel_->setAlignment(Qt::AlignCenter);
    auto* balanceCaption = new QLabel(tr("余额（元）"), balanceButton);
    balanceCaption->setProperty("role", QStringLiteral("walletStatCaption"));
    balanceCaption->setAlignment(Qt::AlignCenter);
    balanceLayout->addWidget(balanceValueLabel_);
    balanceLayout->addWidget(balanceCaption);
    walletLayout->addWidget(balanceButton, 1);

    auto addDivider = [&walletLayout]() {
        auto* divider = new QFrame();
        divider->setProperty("role", QStringLiteral("walletDivider"));
        divider->setFixedWidth(1);
        divider->setFixedHeight(40);
        walletLayout->addWidget(divider, 0, Qt::AlignVCenter);
    };

    auto addStatGlyph = [&](const QString& glyph, const QString& caption,
                            void (ProfilePage::*signal)()) {
        addDivider();
        auto* button = new QPushButton(walletCard);
        button->setProperty("role", QStringLiteral("walletStat"));
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, signal);
        auto* layout = new QVBoxLayout(button);
        layout->setSpacing(4);
        auto* glyphLabel = new QLabel(glyph, button);
        glyphLabel->setProperty("role", QStringLiteral("walletGlyph"));
        glyphLabel->setAlignment(Qt::AlignCenter);
        auto* captionLabel = new QLabel(caption, button);
        captionLabel->setProperty("role", QStringLiteral("walletStatCaption"));
        captionLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(glyphLabel);
        layout->addWidget(captionLabel);
        walletLayout->addWidget(button, 1);
    };
    addStatGlyph(QStringLiteral("💳"), tr("充值"), &ProfilePage::rechargeRequested);
    addStatGlyph(QStringLiteral("🧾"), tr("充值记录"), &ProfilePage::walletRequested);

    walletWrapLayout->addWidget(walletCard);
    rootLayout->addWidget(walletWrap);

    // ---------- 双格入口：我的订单（带待支付角标）/ 我的预约 ----------
    auto* cellsRow = new QHBoxLayout();
    cellsRow->setContentsMargins(16, 14, 16, 0);
    cellsRow->setSpacing(12);

    auto* ordersCell = new QPushButton(this);
    ordersCell->setProperty("role", QStringLiteral("profileCell"));
    ordersCell->setCursor(Qt::PointingHandCursor);
    connect(ordersCell, &QPushButton::clicked, this, &ProfilePage::allOrdersRequested);
    auto* ordersLayout = new QHBoxLayout(ordersCell);
    ordersLayout->setContentsMargins(16, 16, 14, 16);
    ordersLayout->setSpacing(12);
    auto* ordersIcon = new QLabel(QStringLiteral("📋"), ordersCell);
    ordersIcon->setProperty("role", QStringLiteral("cellIcon"));
    ordersLayout->addWidget(ordersIcon);
    auto* ordersText = new QVBoxLayout();
    ordersText->setSpacing(3);
    auto* ordersTitleRow = new QHBoxLayout();
    ordersTitleRow->setSpacing(6);
    auto* ordersTitle = new QLabel(tr("我的订单"), ordersCell);
    ordersTitle->setProperty("role", QStringLiteral("cellTitle"));
    ordersTitleRow->addWidget(ordersTitle);
    ordersTitleRow->addStretch();
    ordersText->addLayout(ordersTitleRow);
    auto* ordersCaption = new QLabel(tr("全部充电订单"), ordersCell);
    ordersCaption->setProperty("role", QStringLiteral("cellCaption"));
    ordersText->addWidget(ordersCaption);
    ordersLayout->addLayout(ordersText);
    ordersLayout->addStretch();
    // 待支付角标：纯数字圆点钉右侧（隐藏时不占位）；不再另放 chevron，
    // 避免标题被挤断。
    waitingBadge_ = new StatusTag(QStringLiteral("0"), StatusTag::Tone::Warning, ordersCell);
    waitingBadge_->setVisible(false);
    waitingBadge_->setToolTip(tr("待支付订单"));
    ordersLayout->addWidget(waitingBadge_);
    cellsRow->addWidget(ordersCell, 1);

    auto* reservationsCell = new QPushButton(this);
    // objectName 沿用壳层测试锚点；样式走 role=profileCell。
    reservationsCell->setObjectName(QStringLiteral("openReservationsButton"));
    reservationsCell->setProperty("role", QStringLiteral("profileCell"));
    reservationsCell->setCursor(Qt::PointingHandCursor);
    connect(reservationsCell, &QPushButton::clicked, this,
            &ProfilePage::reservationRecordsRequested);
    auto* reservationsLayout = new QHBoxLayout(reservationsCell);
    reservationsLayout->setContentsMargins(16, 16, 14, 16);
    reservationsLayout->setSpacing(12);
    auto* reservationsIcon = new QLabel(QStringLiteral("📒"), reservationsCell);
    reservationsIcon->setProperty("role", QStringLiteral("cellIcon"));
    reservationsLayout->addWidget(reservationsIcon);
    auto* reservationsText = new QVBoxLayout();
    reservationsText->setSpacing(3);
    auto* reservationsTitle = new QLabel(tr("我的预约"), reservationsCell);
    reservationsTitle->setProperty("role", QStringLiteral("cellTitle"));
    auto* reservationsCaption = new QLabel(tr("时段预约记录"), reservationsCell);
    reservationsCaption->setProperty("role", QStringLiteral("cellCaption"));
    reservationsText->addWidget(reservationsTitle);
    reservationsText->addWidget(reservationsCaption);
    reservationsLayout->addLayout(reservationsText);
    reservationsLayout->addStretch();
    auto* reservationsChevron = new QLabel(QStringLiteral("›"), reservationsCell);
    reservationsChevron->setProperty("role", QStringLiteral("cellChevron"));
    reservationsLayout->addWidget(reservationsChevron);
    cellsRow->addWidget(reservationsCell, 1);
    rootLayout->addLayout(cellsRow);

    // ---------- 账号与服务（分组列表：设置；成员 2 页面由壳路由承接） ----------
    // root 边距为 0（头图要顶边），以下各段用包裹容器手动留白。
    auto wrapWithMargins = [this, &rootLayout](QWidget* child, int left, int top, int right,
                                               int bottom) {
        auto* wrap = new QWidget(this);
        auto* layout = new QVBoxLayout(wrap);
        layout->setContentsMargins(left, top, right, bottom);
        layout->setSpacing(0);
        layout->addWidget(child);
        rootLayout->addWidget(wrap);
    };

    auto* serviceCaption = new QLabel(tr("账号与服务"), this);
    serviceCaption->setProperty("role", QStringLiteral("sectionTitle"));
    wrapWithMargins(serviceCaption, 18, 22, 16, 8);

    auto* settingsButton = new QPushButton(tr("⚙️　设置　›"), this);
    settingsButton->setObjectName(QStringLiteral("openSettingsButton"));
    settingsButton->setCursor(Qt::PointingHandCursor);
    settingsButton->setMinimumHeight(52);
    connect(settingsButton, &QPushButton::clicked, this, &ProfilePage::settingsRequested);
    wrapWithMargins(settingsButton, 16, 0, 16, 0);

    profileNotice_ = new NoticePanel(QStringLiteral("⚠"), tr("资料加载失败"), QString(),
                                     tr("重试"), this);
    profileNotice_->setVisible(false);
    connect(profileNotice_, &NoticePanel::actionTriggered, this, &ProfilePage::refresh);
    wrapWithMargins(profileNotice_, 16, 14, 16, 0);

    rootLayout->addStretch();

    // 退出登录独立成一行白卡，置底并与日常入口拉开层级。
    auto* logoutButton = new QPushButton(tr("退出登录"), this);
    logoutButton->setObjectName(QStringLiteral("logoutButton"));
    logoutButton->setCursor(Qt::PointingHandCursor);
    logoutButton->setMinimumHeight(50);
    connect(logoutButton, &QPushButton::clicked, this, &ProfilePage::logoutRequested);
    wrapWithMargins(logoutButton, 16, 10, 16, 20);
}

void ProfilePage::refresh()
{
    profileNotice_->setVisible(false);
    walletService_->fetchProfile();
    orderService_->fetchStatusCounts();
}

void ProfilePage::setIdentity(const charging::model::User& user)
{
    renderIdentity(user);
    // 同步渲染真实登录账号；随后 refresh() 会用服务端资料覆盖余额等字段。
}

void ProfilePage::onProfileLoaded(const charging::model::User& user)
{
    profileNotice_->setVisible(false);
    renderIdentity(user);
}

void ProfilePage::renderIdentity(const charging::model::User& user)
{
    user_ = user;
    hasUser_ = true;

    const QString nickname = user_.nickname.isEmpty() ? tr("未设置") : user_.nickname;
    // 内置头像库有 key 时渲染头像图，否则回退昵称首字字母头像。
    AvatarLibrary::applyToLabel(avatarLabel_, user_.avatarKey,
                                nickname.isEmpty() ? QStringLiteral("用")
                                                   : QString(nickname.at(0)),
                                64);
    identityNameLabel_->setText(nickname);
    identityPhoneLabel_->setText(user_.phone);
    balanceValueLabel_->setText(
        QStringLiteral("¥ %1").arg(formatCentsAsYuan(user_.balanceCents)));
}

void ProfilePage::onStatusCounts(int chargingCount, int waitingPaymentCount, int completedCount)
{
    // 双格版式只保留“待支付”这一个必须处理的提醒角标（数字圆点）。
    Q_UNUSED(chargingCount);
    Q_UNUSED(completedCount);
    waitingBadge_->setText(QString::number(waitingPaymentCount));
    waitingBadge_->setVisible(waitingPaymentCount > 0);
}

void ProfilePage::onOperationFailed(const QString& type,
                                    const charging::protocol::ProtocolError& error)
{
    const QString getType =
        QString::fromLatin1(charging::protocol::request_type::kGetUserInfo);
    if (type == getType && !hasUser_) {
        profileNotice_->setContent(QStringLiteral("⚠"), tr("资料加载失败"),
                                   displayMessageForError(error), tr("重试"));
        profileNotice_->setVisible(true);
    }
    // 角标数量失败由 OrderService 静默降级，不上页面。
}

} // namespace charging::client
