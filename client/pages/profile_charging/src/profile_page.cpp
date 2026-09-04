#include "charging/client/profile_charging/profile_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/avatar_library.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/common/protocol/protocol.h"

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
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(14);

    auto* titleLabel = new QLabel(tr("我的"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    rootLayout->addWidget(titleLabel);

    // ---------- 身份头部（点击进入编辑资料页） ----------
    auto* identityCard = new ClickableCard(this);
    connect(identityCard, &ClickableCard::clicked, this, &ProfilePage::profileEditRequested);

    // Mainstream "我的" header: one horizontal row — avatar left, name and
    // phone stacked in the middle, the edit affordance on the right.
    auto* identityRow = new QHBoxLayout();
    identityRow->setSpacing(14);

    avatarLabel_ = new QLabel(QStringLiteral("用"), identityCard);
    avatarLabel_->setObjectName(QStringLiteral("uiAvatarHub"));
    avatarLabel_->setFixedSize(56, 56);
    avatarLabel_->setAlignment(Qt::AlignCenter);
    identityRow->addWidget(avatarLabel_);

    auto* identityText = new QVBoxLayout();
    identityText->setSpacing(2);
    identityNameLabel_ = new QLabel(tr("未登录"), identityCard);
    // objectName 沿用壳层测试锚点（QSS 选择器同步更名）。
    identityNameLabel_->setObjectName(QStringLiteral("nicknameLabel"));
    identityPhoneLabel_ = new QLabel(QStringLiteral("--"), identityCard);
    identityPhoneLabel_->setProperty("role", QStringLiteral("caption"));
    identityText->addWidget(identityNameLabel_);
    identityText->addWidget(identityPhoneLabel_);
    identityRow->addLayout(identityText);

    identityRow->addStretch();

    auto* editHint = new QLabel(tr("编辑资料 ›"), identityCard);
    editHint->setProperty("role", QStringLiteral("secondary"));
    identityRow->addWidget(editHint);
    identityCard->bodyLayout()->addLayout(identityRow);
    rootLayout->addWidget(identityCard);

    // ---------- 钱包（在订单上方，余额直接显示在大圆内） ----------
    auto* walletCard = new Card(this);
    auto* walletLayout = walletCard->bodyLayout();

    auto* walletTitle = new QLabel(tr("钱包"), walletCard);
    walletTitle->setProperty("role", QStringLiteral("sectionTitle"));
    walletLayout->addWidget(walletTitle);

    // Balance banner: WeChat-wallet style rectangle — label top-left, amount
    // below it, stretched across the card instead of a circle.
    auto* balancePanel = new QWidget(walletCard);
    balancePanel->setObjectName(QStringLiteral("uiBalancePanel"));
    balancePanel->setAttribute(Qt::WA_StyledBackground, true);
    auto* balanceLayout = new QVBoxLayout(balancePanel);
    balanceLayout->setContentsMargins(18, 14, 18, 14);
    balanceLayout->setSpacing(2);
    balanceCaptionLabel_ = new QLabel(tr("当前余额（元）"), balancePanel);
    balanceCaptionLabel_->setObjectName(QStringLiteral("uiBalanceCaption"));
    balanceValueLabel_ = new QLabel(QStringLiteral("¥ --"), balancePanel);
    balanceValueLabel_->setObjectName(QStringLiteral("balanceLabel"));
    balanceLayout->addWidget(balanceCaptionLabel_);
    balanceLayout->addWidget(balanceValueLabel_);
    walletLayout->addWidget(balancePanel);

    auto* walletActionRow = new QHBoxLayout();
    walletActionRow->addStretch();
    auto* rechargeButton = new ActionButton(ActionButton::Variant::Primary, tr("充值"), walletCard);
    connect(rechargeButton, &ActionButton::clicked, this, &ProfilePage::rechargeRequested);
    auto* recordsButton =
        new ActionButton(ActionButton::Variant::Secondary, tr("充值记录"), walletCard);
    connect(recordsButton, &ActionButton::clicked, this, &ProfilePage::walletRequested);
    walletActionRow->addWidget(rechargeButton);
    walletActionRow->addWidget(recordsButton);
    walletActionRow->addStretch();
    walletLayout->addLayout(walletActionRow);
    rootLayout->addWidget(walletCard);

    // ---------- 我的订单（入口格子带状态数量角标） ----------
    auto* ordersCard = new Card(this);
    auto* ordersLayout = ordersCard->bodyLayout();

    // 标题独占一行："全部订单"就是第一个格子，不再在标题旁重复放跳转按钮。
    auto* ordersTitle = new QLabel(tr("我的订单"), ordersCard);
    ordersTitle->setProperty("role", QStringLiteral("sectionTitle"));
    ordersLayout->addWidget(ordersTitle);

    auto* gridRow = new QHBoxLayout();
    gridRow->setSpacing(10);

    auto addCell = [&](const QString& glyph, const QString& caption, const QString& tone,
                       QLabel** badgeOut, void (ProfilePage::*signal)()) {
        auto* cell = buildGridCell(glyph, caption, tone, badgeOut);
        connect(cell, &ClickableCard::clicked, this, signal);
        gridRow->addWidget(cell, 1);
    };
    QLabel* allBadge = nullptr;
    addCell(QStringLiteral("☰"), tr("全部订单"), QString(), &allBadge,
            &ProfilePage::allOrdersRequested);
    addCell(QStringLiteral("⚡"), tr("充电中"), QStringLiteral("success"), &chargingBadge_,
            &ProfilePage::chargingOrdersRequested);
    addCell(QStringLiteral("¥"), tr("待支付"), QStringLiteral("warning"), &waitingPaymentBadge_,
            &ProfilePage::waitingPaymentOrdersRequested);
    addCell(QStringLiteral("✔"), tr("已完成"), QStringLiteral("neutral"), &completedBadge_,
            &ProfilePage::completedOrdersRequested);
    Q_UNUSED(allBadge); // 全部订单不带角标：总数没有独立接口。
    ordersLayout->addLayout(gridRow);
    rootLayout->addWidget(ordersCard);

    // ---------- 账号与服务（预约 / 设置；退出登录独立置底） ----------
    auto* moreCard = new Card(this);
    auto* moreLayout = moreCard->bodyLayout();

    auto* moreTitle = new QLabel(tr("账号与服务"), moreCard);
    moreTitle->setProperty("role", QStringLiteral("sectionTitle"));
    moreLayout->addWidget(moreTitle);

    // 电站列表入口已合并进壳层底部 Tab（成员 2 模块），此处只保留预约记录
    // 与设置入口（成员 2 页面已由壳层路由承接）。
    auto* reservationsButton = new QPushButton(tr("📒  我的预约　›"), moreCard);
    reservationsButton->setObjectName(QStringLiteral("openReservationsButton"));
    reservationsButton->setCursor(Qt::PointingHandCursor);
    reservationsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    reservationsButton->setMinimumHeight(44);
    connect(reservationsButton, &QPushButton::clicked, this,
            &ProfilePage::reservationRecordsRequested);
    moreLayout->addWidget(reservationsButton);

    auto* settingsButton = new QPushButton(tr("⚙️  设置　›"), moreCard);
    settingsButton->setObjectName(QStringLiteral("openSettingsButton"));
    settingsButton->setCursor(Qt::PointingHandCursor);
    settingsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    settingsButton->setMinimumHeight(44);
    connect(settingsButton, &QPushButton::clicked, this, &ProfilePage::settingsRequested);
    moreLayout->addWidget(settingsButton);
    rootLayout->addWidget(moreCard);

    profileNotice_ = new NoticePanel(QStringLiteral("⚠"), tr("资料加载失败"), QString(),
                                     tr("重试"), this);
    profileNotice_->setVisible(false);
    connect(profileNotice_, &NoticePanel::actionTriggered, this, &ProfilePage::refresh);
    rootLayout->addWidget(profileNotice_);

    rootLayout->addStretch();

    // 退出登录是全页唯一危险动作：移出卡片、贴底、通栏弱化为文字行，
    // 与日常入口在视觉上拉开层级。
    auto* logoutButton = new QPushButton(tr("退出登录"), this);
    logoutButton->setObjectName(QStringLiteral("logoutButton"));
    logoutButton->setCursor(Qt::PointingHandCursor);
    logoutButton->setMinimumHeight(48);
    connect(logoutButton, &QPushButton::clicked, this, &ProfilePage::logoutRequested);
    rootLayout->addWidget(logoutButton);
}

ClickableCard* ProfilePage::buildGridCell(const QString& glyph, const QString& caption,
                                          const QString& badgeTone, QLabel** badgeOut)
{
    auto* cell = new ClickableCard(this);
    auto* body = cell->bodyLayout();
    body->setSpacing(4);

    // Fixed-height badge row so the icon stays aligned whether or not a
    // count is shown.
    auto* badgeRow = new QWidget(cell);
    auto* badgeRowLayout = new QHBoxLayout(badgeRow);
    badgeRowLayout->setContentsMargins(0, 0, 0, 0);
    badgeRow->setFixedHeight(18);
    auto* badge = new QLabel(badgeRow);
    badge->setObjectName(QStringLiteral("uiBadge"));
    badge->setProperty("tone", badgeTone.isEmpty() ? QStringLiteral("neutral") : badgeTone);
    badge->setAlignment(Qt::AlignCenter);
    badge->setVisible(false);
    badgeRowLayout->addStretch();
    badgeRowLayout->addWidget(badge);
    body->addWidget(badgeRow);

    auto* iconLabel = new QLabel(glyph, cell);
    iconLabel->setProperty("role", QStringLiteral("gridIcon"));
    iconLabel->setAlignment(Qt::AlignCenter);
    body->addWidget(iconLabel, 0, Qt::AlignHCenter);

    auto* captionLabel = new QLabel(caption, cell);
    captionLabel->setProperty("role", QStringLiteral("gridCaption"));
    captionLabel->setAlignment(Qt::AlignCenter);
    body->addWidget(captionLabel, 0, Qt::AlignHCenter);

    *badgeOut = badge;
    return cell;
}

QString ProfilePage::badgeText(int count)
{
    if (count <= 0) {
        return QString();
    }
    return count > 99 ? QStringLiteral("99+") : QString::number(count);
}

void ProfilePage::applyBadge(QLabel* badge, const QString& badgeTone, int count)
{
    const QString text = badgeText(count);
    badge->setText(text);
    badge->setVisible(!text.isEmpty());
    badge->setProperty("tone", badgeTone);
    if (!text.isEmpty()) {
        // Round dot for one digit, pill as it grows; keep the glyph centred.
        const int width = text.length() <= 1 ? 18 : text.length() == 2 ? 22 : 30;
        badge->setFixedSize(width, 18);
    }
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
                                56);
    identityNameLabel_->setText(nickname);
    identityPhoneLabel_->setText(user_.phone);
    balanceValueLabel_->setText(
        QStringLiteral("¥ %1").arg(formatCentsAsYuan(user_.balanceCents)));
}

void ProfilePage::onStatusCounts(int chargingCount, int waitingPaymentCount, int completedCount)
{
    applyBadge(chargingBadge_, QStringLiteral("success"), chargingCount);
    applyBadge(waitingPaymentBadge_, QStringLiteral("warning"), waitingPaymentCount);
    applyBadge(completedBadge_, QStringLiteral("neutral"), completedCount);
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
    // Badge-count failures never surface here (OrderService degrades them
    // silently); failures of other pages' traffic are those pages' business.
}

} // namespace charging::client
