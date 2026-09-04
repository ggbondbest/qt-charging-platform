#include "pages/station/home_shell.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/top_nav_bar.h"
#include "network/client_connection.h"
#include "pages/station/platform_theme.h"
#include "pages/station/reservation_confirm_page.h"
#include "pages/station/reservation_module_page.h"
#include "pages/station/station_detail_page.h"
#include "pages/station/station_home_page.h"
#include "services/reservation/reservation_service.h"

#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
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
        route_ = Route::None;
        topBar_->setBackVisible(false);
        stationPage_->search(keyword);
    });
    // 任务 #12/#17：顶部导航“返回”按钮 → 按当前路由回上一级页面。
    connect(topBar_, &TopNavBar::backRequested, this, &HomeShell::leaveRoute);

    // 内容区：找站 / 订单 / 充值 / 我的。
    pageStack_ = new QStackedWidget(this);
    pageStack_->setObjectName(QStringLiteral("homePageStack"));
    stationPage_ = new StationHomePage(pageStack_);
    pageStack_->addWidget(stationPage_);
    pageStack_->addWidget(createOrderPage());
    pageStack_->addWidget(createRechargePage());
    pageStack_->addWidget(createProfilePage());

    // 站点详情路由页（索引 4，非 Tab 页，任务 #12）：与列表页共用同一
    // 查询服务实例（详情走同一双通道，模拟 ↔ 真实切换 UI 零改动）。
    detailPage_ = new StationDetailPage(pageStack_);
    pageStack_->addWidget(detailPage_);
    detailPage_->setService(stationPage_->service());

    // 任务 #17 迭代：预约服务统一注入详情页（入口拦截）、独立预约确认
    // 页面（索引 5 路由页，替代原弹窗）与“我的预约”模块页（索引 6 路由
    // 页，二级 Tab：预约订单 / 已完成的预约）；登录态与用户 ID 由壳透传。
    reservationService_ = new services::reservation::ReservationService(this);
    if (hasUser_) {
        reservationService_->setUserId(user_.id);
    }
    detailPage_->setReservationService(reservationService_);
    detailPage_->setLoggedIn(hasUser_);
    connect(detailPage_, &StationDetailPage::reservationLoginRequired, this,
            &HomeShell::showReservationLoginPrompt);
    // 业务约束：存在未结束预约时点击预约 → 提示拦截，不进入确认页。
    connect(detailPage_, &StationDetailPage::reservationBlocked, this,
            &HomeShell::showUnfinishedReservationPrompt);
    // 满足条件 → 路由至独立预约确认页面（不再弹窗）。
    connect(detailPage_, &StationDetailPage::reservationConfirmRequested, this,
            &HomeShell::openReservationConfirm);

    confirmPage_ = new ReservationConfirmPage(pageStack_);
    confirmPage_->setService(reservationService_);
    pageStack_->addWidget(confirmPage_);
    // 【关闭】→ 返回站点详情页（顶部导航返回同语义，见 leaveRoute）。
    connect(confirmPage_, &ReservationConfirmPage::closeRequested, this, [this]() {
        pageStack_->setCurrentWidget(detailPage_);
        route_ = Route::StationDetail;
    });
    // 预约成功 → 刷新详情页桩状态并自动路由至预约模块【预约订单】Tab。
    connect(confirmPage_, &ReservationConfirmPage::confirmed, this,
            [this](const services::reservation::ReservationRecord& record) {
                detailPage_->noteChargerReserved(record.reservation.chargerId);
                modulePage_->showOrderTab();
                openReservationModule();
            });

    modulePage_ = new ReservationModulePage(pageStack_);
    modulePage_->setService(reservationService_);
    pageStack_->addWidget(modulePage_);
    // 订单页空态“去找桩” → 回“找站”Tab（路由页不改 Tab 选中，显式落栈）。
    connect(modulePage_, &ReservationModulePage::findStationRequested, this, [this]() {
        tabBar_->setCurrentTab(QStringLiteral("station"));
        pageStack_->setCurrentIndex(0);
        route_ = Route::None;
        topBar_->setBackVisible(false);
    });

    connect(stationPage_, &StationHomePage::stationSelected, this,
            &HomeShell::openStationDetail);
    connect(detailPage_, &StationDetailPage::backRequested, this, &HomeShell::leaveRoute);
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
        // 切任意 Tab 即离开详情/预约记录路由页：收起顶部导航返回按钮。
        route_ = Route::None;
        topBar_->setBackVisible(false);
    }
}

void HomeShell::openStationDetail(const charging::model::Station& station, int distanceMeters)
{
    // 路由携带站点快照（含站点 ID）：非法/缺失 ID 由服务详情通道回错误态。
    detailPage_->openStation(station, distanceMeters);
    pageStack_->setCurrentWidget(detailPage_);
    // 复用全局顶部导航的返回按钮回列表（不重复开发页面级导航）。
    topBar_->setBackVisible(true);
    route_ = Route::StationDetail;
}

void HomeShell::openReservationConfirm(const charging::model::Station& station,
                                       const charging::model::Charger& charger,
                                       int distanceMeters)
{
    // 独立预约确认页面路由（任务 #17 迭代，替代弹窗）：由详情页在满足
    // 预约条件（已登录 + 无未结束预约）后发信号进入；返回目标为详情页，
    // 顶部导航返回按钮保持可见（见 leaveRoute 的 ReservationConfirm 分支）。
    confirmPage_->openContext(station, charger, distanceMeters);
    pageStack_->setCurrentWidget(confirmPage_);
    topBar_->setBackVisible(true);
    route_ = Route::ReservationConfirm;
}

void HomeShell::openReservationModule()
{
    if (!hasUser_) {
        // 未登录访问“我的预约”：提示登录后跳登录页（沿用全局登录态）。
        showReservationLoginPrompt();
        return;
    }
    modulePage_->refresh();
    pageStack_->setCurrentWidget(modulePage_);
    topBar_->setBackVisible(true);
    route_ = Route::ReservationModule;
}

void HomeShell::leaveRoute()
{
    // 顶部导航/页面返回统一出口：预约确认页 → 站点详情（保留详情路由），
    // 预约模块 → “我的”，详情页 → 找站列表，并收起返回按钮。Tab 可能
    // 本就处于选中态（路由页不改变 Tab 选中），因此除切 Tab 外显式落回
    // 内容栈。
    if (route_ == Route::ReservationConfirm) {
        pageStack_->setCurrentWidget(detailPage_);
        route_ = Route::StationDetail;
        return;
    }
    const bool toProfile = route_ == Route::ReservationModule;
    route_ = Route::None;
    topBar_->setBackVisible(false);
    tabBar_->setCurrentTab(toProfile ? QStringLiteral("profile")
                                     : QStringLiteral("station"));
    pageStack_->setCurrentIndex(toProfile ? 3 : 0);
}

void HomeShell::showReservationLoginPrompt()
{
    // 登录拦截提示（任务 #17）：非模态确认框，“去登录”经全局 loginRequested
    // 由宿主跳登录页；不重复实现登录逻辑。
    auto* prompt = new QMessageBox(this);
    prompt->setObjectName(QStringLiteral("reservationLoginPrompt"));
    prompt->setIcon(QMessageBox::Warning);
    prompt->setWindowTitle(tr("需要登录"));
    prompt->setText(tr("预约充电桩需要先登录账号。"));
    prompt->setInformativeText(tr("登录后即可预约充电桩并查看我的预约记录。"));
    QPushButton* goLogin =
        prompt->addButton(tr("去登录"), QMessageBox::AcceptRole);
    goLogin->setObjectName(QStringLiteral("reservationGoLoginButton"));
    prompt->addButton(tr("稍后再说"), QMessageBox::RejectRole);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    connect(prompt, &QMessageBox::finished, this, [this, prompt, goLogin](int) {
        if (prompt->clickedButton() == goLogin) {
            emit loginRequested();
        }
    });
    prompt->open();
}

void HomeShell::showUnfinishedReservationPrompt()
{
    // 业务约束提示（任务 #17 迭代）：存在未结束预约时禁止新建。非模态，
    // “去查看”直达预约模块【预约订单】Tab 结束当前预约。
    auto* prompt = new QMessageBox(this);
    prompt->setObjectName(QStringLiteral("unfinishedReservationPrompt"));
    prompt->setIcon(QMessageBox::Warning);
    prompt->setWindowTitle(tr("无法发起新预约"));
    prompt->setText(tr("您当前尚有未结束的预约，请结束当前预约后再发起新预约"));
    QPushButton* goLook = prompt->addButton(tr("去查看"), QMessageBox::AcceptRole);
    goLook->setObjectName(QStringLiteral("unfinishedGoLookButton"));
    prompt->addButton(tr("知道了"), QMessageBox::RejectRole);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    connect(prompt, &QMessageBox::finished, this, [this, prompt, goLook](int) {
        if (prompt->clickedButton() == goLook) {
            modulePage_->showOrderTab();
            openReservationModule();
        }
    });
    prompt->open();
}

void HomeShell::setConnection(charging::client::network::ClientConnection* connection)
{
    // 页面构造时无需连接即可用模拟数据渲染；注入后保留真实接口通道，
    // 服务端 GET_STATIONS 就绪时开启 liveMode 即无缝切换（UI 逻辑不变）。
    stationPage_->service()->setConnection(connection);
    reservationService_->setConnection(connection);
}

StationHomePage* HomeShell::stationPage() const
{
    return stationPage_;
}

ReservationModulePage* HomeShell::reservationModule() const
{
    return modulePage_;
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
        // 未登录也可点“我的预约”入口：由壳统一拦截并提示登录（任务 #17）。
        auto* reservationsButton = new QPushButton(tr("📒 我的预约"), card);
        reservationsButton->setObjectName(QStringLiteral("openReservationsButton"));
        reservationsButton->setCursor(Qt::PointingHandCursor);
        connect(reservationsButton, &QPushButton::clicked, this,
                &HomeShell::openReservationModule);
        body->addStretch();
        body->addWidget(notice);
        body->addWidget(reservationsButton, 0, Qt::AlignCenter);
        body->addStretch();
        outer->addStretch();
        outer->addWidget(card);
        outer->addStretch();
        return wrapper;
    }

    // 任务 #17 迭代：个人中心布局重构——用户信息居上，功能容器垂直排布，
    // 退出登录红色字体置于最底部；内容超出可视区域支持鼠标滚轮滚动。
    auto* scroll = new QScrollArea(pageStack_);
    scroll->setObjectName(QStringLiteral("profileScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QPushButton[isDangerText=\"true\"] {"
        " background: transparent; border: none; color: #E5484D;"
        " font-size: 14px; font-weight: 700; padding: 10px;"
        " }"
        "QPushButton[isDangerText=\"true\"]:hover { color: #C13438; }"));

    auto* wrapper = new QWidget(scroll);
    auto* outer = new QVBoxLayout(wrapper);
    outer->setContentsMargins(20, 16, 20, 16);
    outer->setSpacing(10);
    scroll->setWidget(wrapper);

    // ① 用户信息（保留在页面上方）：昵称、手机号、余额。
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
    outer->addWidget(card);

    // ② 功能区（自上而下垂直排布）。我的预约入口 → 预约模块二级 Tab。
    auto* reservationsCard = new Card(wrapper);
    reservationsCard->setProperty("isFunctionSlot", true);
    auto* reservationsBody = reservationsCard->bodyLayout();
    auto* reservationsButton = new QPushButton(tr("📒 我的预约（预约订单 / 已完成的预约）"),
                                               reservationsCard);
    reservationsButton->setObjectName(QStringLiteral("openReservationsButton"));
    reservationsButton->setCursor(Qt::PointingHandCursor);
    connect(reservationsButton, &QPushButton::clicked, this,
            &HomeShell::openReservationModule);
    reservationsBody->addWidget(reservationsButton);
    outer->addWidget(reservationsCard);

    // 预留功能容器（仅占位，业务逻辑暂不实现）。
    const auto makeSlot = [&](const QString& title) {
        auto* slot = new Card(wrapper);
        slot->setProperty("isFunctionSlot", true);
        auto* row = new QHBoxLayout();
        auto* titleLabel = new QLabel(title, slot);
        titleLabel->setProperty("role", QStringLiteral("sectionTitle"));
        auto* hint = new QLabel(tr("敬请期待"), slot);
        hint->setProperty("role", QStringLiteral("caption"));
        row->addWidget(titleLabel);
        row->addStretch();
        row->addWidget(hint);
        slot->bodyLayout()->addLayout(row);
        outer->addWidget(slot);
    };
    makeSlot(tr("⚡ 充电订单"));
    makeSlot(tr("🎫 优惠券"));
    makeSlot(tr("⚙️ 设置"));

    // ③ 所有容器最底部：退出登录（红色字体，规格要求）。
    auto* logoutButton = new QPushButton(tr("退出登录"), wrapper);
    logoutButton->setObjectName(QStringLiteral("logoutButton"));
    logoutButton->setProperty("isDangerText", true);
    logoutButton->setCursor(Qt::PointingHandCursor);
    connect(logoutButton, &QPushButton::clicked, this,
            [this]() { emit logoutRequested(); });
    outer->addWidget(logoutButton);
    outer->addStretch();
    return scroll;
}

} // namespace charging::client::pages::station
