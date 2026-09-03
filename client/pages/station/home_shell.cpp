#include "pages/station/home_shell.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/top_nav_bar.h"
#include "network/client_connection.h"
#include "pages/station/platform_theme.h"
#include "pages/station/station_detail_page.h"
#include "pages/station/station_home_page.h"

#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

// 占位页（订单 / 充值）：卡片内说明槽位与后续负责人，保持统一导航样式。
QWidget* makePlaceholderPage(const QString& glyph, const QString& title, const QString& hint,
                             QWidget* parent)
{
    auto* wrapper = new QWidget(parent);
    auto* outer = new QVBoxLayout(wrapper);
    outer->setContentsMargins(20, 16, 20, 16);

    auto* card = new Card(wrapper);
    card->setProperty("isPlaceholderCard", true);
    auto* body = card->bodyLayout();

    auto* glyphLabel = new QLabel(glyph, card);
    glyphLabel->setAlignment(Qt::AlignCenter);
    glyphLabel->setProperty("role", QStringLiteral("successCheck"));

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));

    auto* hintLabel = new QLabel(hint, card);
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setWordWrap(true);
    hintLabel->setProperty("role", QStringLiteral("secondary"));

    body->addStretch();
    body->addWidget(glyphLabel);
    body->addWidget(titleLabel);
    body->addWidget(hintLabel);
    body->addStretch();

    outer->addStretch();
    outer->addWidget(card);
    outer->addStretch();
    return wrapper;
}

} // namespace

HomeShell::HomeShell(const charging::model::User& user, QWidget* parent)
    : HomeShell(&user, parent)
{
}

HomeShell::HomeShell(QWidget* parent) : HomeShell(nullptr, parent)
{
}

HomeShell::HomeShell(const charging::model::User* user, QWidget* parent) : QWidget(parent)
{
    // 页面可能在测试/预览中独立构造；主题安装是幂等的。
    installPlatformTheme();

    setObjectName(QStringLiteral("homeShell"));
    hasUser_ = (user != nullptr);
    if (hasUser_) {
        user_ = *user;
    }

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 顶部导航公共组件：Logo+平台名 / 搜索框 / 登录态动态右侧。
    topBar_ = new TopNavBar(this);
    if (hasUser_) {
        topBar_->setUser(user_);
    } else {
        topBar_->clearUser();
    }
    rootLayout->addWidget(topBar_);

    connect(topBar_, &TopNavBar::profileRequested, this,
            [this]() { tabBar_->setCurrentTab(QStringLiteral("profile")); });
    connect(topBar_, &TopNavBar::loginRequested, this,
            [this]() { emit loginRequested(); });
    // 任务 #7：顶部搜索框 → 回到“找站”并触发站点检索（含加载状态）。
    connect(topBar_, &TopNavBar::searchSubmitted, this, [this](const QString& keyword) {
        tabBar_->setCurrentTab(QStringLiteral("station"));
        pageStack_->setCurrentIndex(0);
        stationPage_->search(keyword);
    });

    // 内容区：找站 / 订单 / 充值 / 我的。
    pageStack_ = new QStackedWidget(this);
    pageStack_->setObjectName(QStringLiteral("homePageStack"));
    stationPage_ = new StationHomePage(pageStack_);
    pageStack_->addWidget(stationPage_);
    pageStack_->addWidget(createOrderPage());
    pageStack_->addWidget(createRechargePage());
    pageStack_->addWidget(createProfilePage());

    // 站点详情路由页（索引 4，非 Tab 页）：任务 #7 仅跳转，业务属任务 #12。
    detailPage_ = new StationDetailPage(pageStack_);
    pageStack_->addWidget(detailPage_);
    connect(stationPage_, &StationHomePage::stationSelected, this,
            &HomeShell::openStationDetail);
    connect(detailPage_, &StationDetailPage::backRequested, this, [this]() {
        pageStack_->setCurrentIndex(0);
        tabBar_->setCurrentTab(QStringLiteral("station"));
    });
    rootLayout->addWidget(pageStack_, 1);

    // 底部 Tab 导航公共组件：固定底部，四个 Tab。
    tabBar_ = new BottomTabBar({{QStringLiteral("station"), tr("🔍 找站")},
                                {QStringLiteral("order"), tr("📋 订单")},
                                {QStringLiteral("recharge"), tr("💰 充值")},
                                {QStringLiteral("profile"), tr("👤 我的")}},
                               this);
    rootLayout->addWidget(tabBar_);

    connect(tabBar_, &BottomTabBar::tabChanged, this, &HomeShell::showTab);
    // 默认激活“找站”（首页）。
    tabBar_->setCurrentTab(QStringLiteral("station"));
}

void HomeShell::showTab(const QString& id)
{
    static const QHash<QString, int> kIndexById{
        {QStringLiteral("station"), 0},   {QStringLiteral("order"), 1},
        {QStringLiteral("recharge"), 2}, {QStringLiteral("profile"), 3},
    };
    const auto index = kIndexById.value(id, -1);
    if (index >= 0) {
        pageStack_->setCurrentIndex(index);
    }
}

void HomeShell::openStationDetail(const charging::model::Station& station, int distanceMeters)
{
    // 任务 #7 只打通路由；详情业务由任务 #12 在 StationDetailPage 内实现。
    detailPage_->openStation(station, distanceMeters);
    pageStack_->setCurrentWidget(detailPage_);
}

void HomeShell::setConnection(charging::client::network::ClientConnection* connection)
{
    // 页面构造时无需连接即可用模拟数据渲染；注入后保留真实接口通道，
    // 服务端 GET_STATIONS 就绪时开启 liveMode 即无缝切换（UI 逻辑不变）。
    stationPage_->service()->setConnection(connection);
}

StationHomePage* HomeShell::stationPage() const
{
    return stationPage_;
}

QWidget* HomeShell::createOrderPage()
{
    return makePlaceholderPage(QStringLiteral("📋"), tr("订单"),
                               tr("订单列表与结算详情成员 3 已实现（OrderHistoryPage），"
                                  "等待查询接口联调后替换本占位页。"),
                               pageStack_);
}

QWidget* HomeShell::createRechargePage()
{
    return makePlaceholderPage(QStringLiteral("💰"), tr("充值"),
                               tr("充值入口与钱包页成员 3 已实现（WalletPage），"
                                  "等待支付通道接口接入后替换本占位页。"),
                               pageStack_);
}

QWidget* HomeShell::createProfilePage()
{
    if (!hasUser_) {
        // 未登录异常路径：进入“我的”先提示登录，点击立即登录回登录页。
        auto* wrapper = new QWidget(pageStack_);
        auto* outer = new QVBoxLayout(wrapper);
        outer->setContentsMargins(20, 16, 20, 16);
        auto* card = new Card(wrapper);
        auto* body = card->bodyLayout();
        auto* notice = new NoticePanel(QStringLiteral("👤"), tr("尚未登录"),
                                       tr("登录后可查看账户信息与个人中心。"),
                                       tr("立即登录"), card);
        connect(notice, &NoticePanel::actionTriggered, this,
                [this]() { emit loginRequested(); });
        body->addStretch();
        body->addWidget(notice);
        body->addStretch();
        outer->addStretch();
        outer->addWidget(card);
        outer->addStretch();
        return wrapper;
    }

    auto* wrapper = new QWidget(pageStack_);
    auto* outer = new QVBoxLayout(wrapper);
    outer->setContentsMargins(20, 16, 20, 16);
    outer->setSpacing(10);

    // 账户卡片：昵称、手机号、余额与退出登录（登录态透传的落点）。
    auto* card = new Card(wrapper);
    card->setProperty("isProfileCard", true);
    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    auto* avatarLabel = new QLabel(QStringLiteral("👤"), card);
    avatarLabel->setObjectName(QStringLiteral("profileAvatarLabel"));
    auto* nameLabel = new QLabel(tr("你好，%1").arg(user_.nickname), card);
    nameLabel->setObjectName(QStringLiteral("nicknameLabel"));
    nameLabel->setProperty("role", QStringLiteral("sectionTitle"));
    auto* loginTag = new StatusTag(tr("已登录"), StatusTag::Tone::Success, card);
    titleRow->addWidget(avatarLabel);
    titleRow->addWidget(nameLabel);
    titleRow->addStretch();
    titleRow->addWidget(loginTag);
    body->addLayout(titleRow);

    auto* phoneLabel = new QLabel(tr("手机号：%1").arg(user_.phone), card);
    phoneLabel->setObjectName(QStringLiteral("phoneLabel"));
    phoneLabel->setProperty("role", QStringLiteral("secondary"));
    body->addWidget(phoneLabel);

    auto* balanceLabel = new QLabel(
        tr("余额：%1 元").arg(QString::number(user_.balanceCents / 100.0, 'f', 2)), card);
    balanceLabel->setObjectName(QStringLiteral("balanceLabel"));
    balanceLabel->setProperty("role", QStringLiteral("amountStrong"));
    body->addWidget(balanceLabel);

    auto* logoutButton = new QPushButton(tr("退出登录"), card);
    logoutButton->setObjectName(QStringLiteral("logoutButton"));
    logoutButton->setCursor(Qt::PointingHandCursor);
    connect(logoutButton, &QPushButton::clicked, this,
            [this]() { emit logoutRequested(); });
    body->addWidget(logoutButton, 0, Qt::AlignRight);

    auto* hintLabel = new QLabel(tr("充电订单、优惠券与设置入口将在后续任务并入本页。"),
                                 card);
    hintLabel->setWordWrap(true);
    hintLabel->setProperty("role", QStringLiteral("caption"));
    body->addWidget(hintLabel);

    outer->addWidget(card);
    outer->addStretch();
    return wrapper;
}

} // namespace charging::client::pages::station
