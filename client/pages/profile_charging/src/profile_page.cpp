#include "charging/client/profile_charging/profile_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/common/protocol/protocol.h"

#include <QHBoxLayout>
#include <QLabel>
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
    identityNameLabel_->setObjectName(QStringLiteral("uiIdentityName"));
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
    balanceValueLabel_->setObjectName(QStringLiteral("uiBalanceValue"));
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

    auto* ordersHeader = new QHBoxLayout();
    auto* ordersTitle = new QLabel(tr("我的订单"), ordersCard);
    ordersTitle->setProperty("role", QStringLiteral("sectionTitle"));
    auto* allOrdersButton =
        new ActionButton(ActionButton::Variant::Ghost, tr("全部订单 ›"), ordersCard);
    allOrdersButton->setStyleSheet(QStringLiteral("font-size:12px;"));
    connect(allOrdersButton, &ActionButton::clicked, this, &ProfilePage::allOrdersRequested);
    ordersHeader->addWidget(ordersTitle);
    ordersHeader->addStretch();
    ordersHeader->addWidget(allOrdersButton);
    ordersLayout->addLayout(ordersHeader);

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
    addCell(QStringLiteral("¥"), tr("待支付"), QStringLiteral("danger"), &waitingPaymentBadge_,
            &ProfilePage::waitingPaymentOrdersRequested);
    addCell(QStringLiteral("✔"), tr("已完成"), QStringLiteral("neutral"), &completedBadge_,
            &ProfilePage::completedOrdersRequested);
    Q_UNUSED(allBadge); // 全部订单不带角标：总数没有独立接口。
    ordersLayout->addLayout(gridRow);
    rootLayout->addWidget(ordersCard);

    // ---------- 更多服务（其他成员的模块，占位禁用，不做假功能） ----------
    auto* moreCard = new Card(this);
    auto* moreLayout = moreCard->bodyLayout();

    auto* moreTitle = new QLabel(tr("更多服务"), moreCard);
    moreTitle->setProperty("role", QStringLiteral("sectionTitle"));
    moreLayout->addWidget(moreTitle);

    auto addPlaceholderRow = [&](const QString& name, const QString& owner) {
        auto* row = new QWidget(moreCard);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setProperty("role", QStringLiteral("subtitle"));
        auto* hintLabel = new QLabel(owner, row);
        hintLabel->setProperty("role", QStringLiteral("caption"));
        rowLayout->addWidget(nameLabel);
        rowLayout->addStretch();
        rowLayout->addWidget(hintLabel);
        moreLayout->addWidget(row);
    };
    addPlaceholderRow(tr("电站列表"), tr("待成员2 模块开放"));
    addPlaceholderRow(tr("设置"), tr("待开放"));
    rootLayout->addWidget(moreCard);

    profileNotice_ = new NoticePanel(QStringLiteral("⚠"), tr("资料加载失败"), QString(),
                                     tr("重试"), this);
    profileNotice_->setVisible(false);
    connect(profileNotice_, &NoticePanel::actionTriggered, this, &ProfilePage::refresh);
    rootLayout->addWidget(profileNotice_);

    rootLayout->addStretch();
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

void ProfilePage::onProfileLoaded(const charging::model::User& user)
{
    user_ = user;
    hasUser_ = true;
    profileNotice_->setVisible(false);

    const QString nickname = user_.nickname.isEmpty() ? tr("未设置") : user_.nickname;
    avatarLabel_->setText(nickname.isEmpty() ? QStringLiteral("用") : QString(nickname.at(0)));
    identityNameLabel_->setText(nickname);
    identityPhoneLabel_->setText(user_.phone);
    balanceValueLabel_->setText(
        QStringLiteral("¥ %1").arg(formatCentsAsYuan(user_.balanceCents)));
}

void ProfilePage::onStatusCounts(int chargingCount, int waitingPaymentCount, int completedCount)
{
    applyBadge(chargingBadge_, QStringLiteral("success"), chargingCount);
    applyBadge(waitingPaymentBadge_, QStringLiteral("danger"), waitingPaymentCount);
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
