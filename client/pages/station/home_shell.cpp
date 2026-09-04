#include "pages/station/home_shell.h"

#include "charging/client/profile_charging/charging_page.h"
#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/order_detail_page.h"
#include "charging/client/profile_charging/order_list_page.h"
#include "charging/client/profile_charging/profile_edit_page.h"
#include "charging/client/profile_charging/profile_page.h"
#include "charging/client/profile_charging/recharge_page.h"
#include "charging/client/profile_charging/settlement_page.h"
#include "charging/client/profile_charging/wallet_page.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"
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

// Tab id → 内容栈固定索引（0–3 为 Tab 页；路由页自 4 起追加）。
const QHash<QString, int>& tabIndexById()
{
    static const QHash<QString, int> kIndexById{
        {QStringLiteral("station"), 0},   {QStringLiteral("order"), 1},
        {QStringLiteral("recharge"), 2}, {QStringLiteral("profile"), 3},
    };
    return kIndexById;
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
        backTargets_.clear();
        topBar_->setBackVisible(false);
        stationPage_->search(keyword);
    });
    // 任务 #12/#17：顶部导航“返回”按钮 → 按当前路由回上一级页面。
    connect(topBar_, &TopNavBar::backRequested, this, &HomeShell::leaveRoute);

    // 内容区：找站 / 订单 / 充值 / 我的。
    // 全端整合：登录态下三个占位 Tab 换成成员 3 真实页面；未登录保持
    // 占位（不向游客暴露 mock 数据）。
    pageStack_ = new QStackedWidget(this);
    pageStack_->setObjectName(QStringLiteral("homePageStack"));
    stationPage_ = new StationHomePage(pageStack_);
    pageStack_->addWidget(stationPage_);
    if (hasUser_) {
        // 共享 mock 通道：播种真实登录账号，钱包/资料/订单展示同一身份；
        // 服务端补齐钱包族命令后经服务层切换（TODO(contract)）。
        mockTransport_ = new MockRequestTransport();
        mockTransport_->setParent(this);
        mockTransport_->setUser(user_);
        walletService_ = new WalletService(mockTransport_, this);
        orderService_ = new OrderService(mockTransport_, this);
        chargingService_ = new ChargingService(mockTransport_, this);
        lastKnownBalanceCents_ = user_.balanceCents;
        connect(walletService_, &WalletService::profileLoaded, this,
                [this](const charging::model::User& profile) {
                    lastKnownBalanceCents_ = profile.balanceCents;
                });
        connect(chargingService_, &ChargingService::paymentCompleted, this,
                [this](qint64, qint64 balanceAfterCents) {
                    lastKnownBalanceCents_ = balanceAfterCents;
                });

        orderListPage_ = new OrderListPage(orderService_, pageStack_);
        walletPage_ = new WalletPage(walletService_, pageStack_);
        profilePage_ = new ProfilePage(walletService_, orderService_, pageStack_);
        // 同步渲染真实身份/余额，异步 fetch 回来后覆盖为服务端值。
        profilePage_->setIdentity(user_);
        // 壳层内成员 3 页面隐藏页内返回/跳转入口（全局导航与底部 Tab 已覆盖）。
        orderListPage_->setEmbedded(true);
        walletPage_->setEmbedded(true);
    }
    pageStack_->addWidget(hasUser_ ? static_cast<QWidget*>(orderListPage_)
                                   : createOrderPage());
    pageStack_->addWidget(hasUser_ ? static_cast<QWidget*>(walletPage_)
                                   : createRechargePage());
    pageStack_->addWidget(hasUser_ ? static_cast<QWidget*>(profilePage_)
                                   : createProfilePage());

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
    // 【关闭】→ 返回站点详情页（顶部导航返回同语义：确认页的返回目标
    // 正是入页时记录的详情页）。
    connect(confirmPage_, &ReservationConfirmPage::closeRequested, this,
            &HomeShell::leaveRoute);
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
        backTargets_.clear();
        topBar_->setBackVisible(false);
    });

    // 全端整合：成员 3 路由页（索引 7–11，追加在既有路由页之后，不影响
    // 成员 2 的 4–6 索引约定）与跨模块导航接线。充电中订单点击进入
    // ChargingPage（停止充电 → 结算闭环，索引 11）；服务端命令就绪后
    // 共享 mock 通道整体切换（TODO(contract)）。
    if (hasUser_) {
        orderDetailPage_ = new OrderDetailPage(pageStack_);
        pageStack_->addWidget(orderDetailPage_);      // 7
        settlementPage_ = new SettlementPage(chargingService_, pageStack_);
        pageStack_->addWidget(settlementPage_);       // 8
        rechargePage_ = new RechargePage(walletService_, pageStack_);
        pageStack_->addWidget(rechargePage_);         // 9
        profileEditPage_ = new ProfileEditPage(walletService_, pageStack_);
        pageStack_->addWidget(profileEditPage_);      // 10
        chargingPage_ = new ChargingPage(chargingService_, pageStack_);
        pageStack_->addWidget(chargingPage_);         // 11

        // 路由页统一隐藏页内返回：返回动作由顶部导航全局承担。
        orderDetailPage_->setEmbedded(true);
        settlementPage_->setEmbedded(true);
        rechargePage_->setEmbedded(true);
        profileEditPage_->setEmbedded(true);
        chargingPage_->setEmbedded(true);

        // 订单 Tab：列表点行 → 详情路由。
        connect(orderListPage_, &OrderListPage::orderOpened, this,
                &HomeShell::openOrderDetail);

        // 充值 Tab（钱包/充值记录）：横向导航回壳层 Tab 与充值路由。
        connect(walletPage_, &WalletPage::rechargeRequested, this, &HomeShell::openRecharge);
        connect(walletPage_, &WalletPage::ordersRequested, this, [this]() {
            tabBar_->setCurrentTab(QStringLiteral("order"));
        });
        connect(walletPage_, &WalletPage::profileRequested, this, [this]() {
            tabBar_->setCurrentTab(QStringLiteral("profile"));
        });

        // “我的”中心页：入口信号 → 路由页 / Tab / 成员 2 预约记录。
        connect(profilePage_, &ProfilePage::profileEditRequested, this,
                &HomeShell::openProfileEdit);
        connect(profilePage_, &ProfilePage::walletRequested, this, [this]() {
            tabBar_->setCurrentTab(QStringLiteral("recharge"));
        });
        connect(profilePage_, &ProfilePage::rechargeRequested, this, &HomeShell::openRecharge);
        connect(profilePage_, &ProfilePage::allOrdersRequested, this, [this]() {
            pendingOrderFilter_ = charging::client::OrderService::Filter::All;
            tabBar_->setCurrentTab(QStringLiteral("order"));
        });
        connect(profilePage_, &ProfilePage::chargingOrdersRequested, this, [this]() {
            pendingOrderFilter_ = charging::client::OrderService::Filter::Charging;
            tabBar_->setCurrentTab(QStringLiteral("order"));
        });
        connect(profilePage_, &ProfilePage::waitingPaymentOrdersRequested, this, [this]() {
            pendingOrderFilter_ = charging::client::OrderService::Filter::WaitingPayment;
            tabBar_->setCurrentTab(QStringLiteral("order"));
        });
        connect(profilePage_, &ProfilePage::completedOrdersRequested, this, [this]() {
            pendingOrderFilter_ = charging::client::OrderService::Filter::Completed;
            tabBar_->setCurrentTab(QStringLiteral("order"));
        });
        connect(profilePage_, &ProfilePage::reservationRecordsRequested, this,
                &HomeShell::openReservationModule);
        connect(profilePage_, &ProfilePage::logoutRequested, this,
                [this]() { emit logoutRequested(); });

        // 路由页内部返回按钮与顶部导航返回共用 leaveRoute。
        connect(orderDetailPage_, &OrderDetailPage::backRequested, this, &HomeShell::leaveRoute);
        connect(orderDetailPage_, &OrderDetailPage::payRequested, this,
                &HomeShell::openSettlement);
        connect(settlementPage_, &SettlementPage::backRequested, this, &HomeShell::leaveRoute);
        connect(settlementPage_, &SettlementPage::rechargeRequested, this,
                &HomeShell::openRecharge);
        connect(settlementPage_, &SettlementPage::doneRequested, this, [this]() {
            // 支付完成 → 订单 Tab 看结果（按已完成筛选）。当前 Tab 通常已是
            // order（详情/结算只是叠在上面的路由页），setCurrentTab 同 id 不
            // 发信号，必须手动 showTab 才会清返回栈并触发筛选刷新。
            pendingOrderFilter_ = charging::client::OrderService::Filter::Completed;
            const bool alreadyCurrent = tabBar_->currentTab() == QLatin1String("order");
            tabBar_->setCurrentTab(QStringLiteral("order"));
            if (alreadyCurrent) {
                showTab(QStringLiteral("order"));
            }
        });
        connect(chargingPage_, &ChargingPage::backRequested, this, &HomeShell::leaveRoute);
        connect(chargingPage_, &ChargingPage::settlementRequested, this,
                [this](const charging::client::ChargingStatus& stopped) {
                    // 会话已结束：先退出“充电过程”路由（已结束会话不应是返回
                    // 目的地），再压入结算页（此时栈顶出发点是订单 Tab）。
                    leaveRoute();
                    settlementPage_->showOrder(stopped);
                    settlementPage_->setBalance(lastKnownBalanceCents_);
                    pushRoute(settlementPage_);
                });
        connect(rechargePage_, &RechargePage::backRequested, this, &HomeShell::leaveRoute);
        connect(rechargePage_, &RechargePage::rechargeSucceeded, this,
                [this](qint64 balanceAfterCents) {
                    lastKnownBalanceCents_ = balanceAfterCents;
                    // 余额不足 → 去充值 → 充值成功后自动回跳入口页。结算页
                    // 显示时用的是进页快照余额，必须把权威新余额推给它，
                    // 否则“确认支付”仍按不足置灰（回跳后支付不了的根因）。
                    settlementPage_->setBalance(balanceAfterCents);
                    leaveRoute();
                    // 充值页回跳后不可见，成功反馈由壳层在目的地上方展示。
                    Toast::show(this, tr("充值成功，余额已更新"), StatusTag::Tone::Success);
                });
        connect(profileEditPage_, &ProfileEditPage::backRequested, this,
                &HomeShell::leaveRoute);
    }

    connect(stationPage_, &StationHomePage::stationSelected, this,
            &HomeShell::openStationDetail);
    connect(detailPage_, &StationDetailPage::backRequested, this, &HomeShell::leaveRoute);
    rootLayout->addWidget(pageStack_, 1);

    // 底部 Tab 导航公共组件：固定底部，四个 Tab。
    // “充值”Tab 挂载的是钱包页（余额+充值+流水），文案改为“钱包”更贴合内容；
    // Tab id 保持 recharge，不动成员 2 的测试锚点。
    tabBar_ = new BottomTabBar({{QStringLiteral("station"), tr("🔍 找站")},
                                {QStringLiteral("order"), tr("📋 订单")},
                                {QStringLiteral("recharge"), tr("💰 钱包")},
                                {QStringLiteral("profile"), tr("👤 我的")}},
                               this);
    rootLayout->addWidget(tabBar_);

    connect(tabBar_, &BottomTabBar::tabChanged, this, &HomeShell::showTab);
    // 默认激活“找站”（首页）。
    tabBar_->setCurrentTab(QStringLiteral("station"));
}

void HomeShell::showTab(const QString& id)
{
    const auto index = tabIndexById().value(id, -1);
    if (index < 0) {
        return;
    }
    pageStack_->setCurrentIndex(index);
    // 切任意 Tab 即离开路由页：清返回栈、收起顶部导航返回按钮。
    backTargets_.clear();
    topBar_->setBackVisible(false);
    // 全端整合：成员 3 Tab 页进入即重查，展示最新的（mock）服务端结果。
    if (id == QLatin1String("order") && orderListPage_) {
        if (pendingOrderFilter_) {
            orderListPage_->showFilter(*pendingOrderFilter_);
            pendingOrderFilter_.reset();
        } else {
            orderListPage_->refresh();
        }
    } else if (id == QLatin1String("recharge") && walletPage_) {
        walletPage_->refresh();
    } else if (id == QLatin1String("profile") && profilePage_) {
        profilePage_->refresh();
    }
}

void HomeShell::pushRoute(QWidget* page)
{
    // 记录出发位置：Tab 页记 tabId（返回时切 Tab 顺带清栈），路由页记
    // page（支持“列表→详情→结算”等多级返回）。
    auto* current = pageStack_->currentWidget();
    BackTarget target;
    const int index = pageStack_->indexOf(current);
    if (index >= 0 && index <= 3) {
        for (auto it = tabIndexById().cbegin(); it != tabIndexById().cend(); ++it) {
            if (it.value() == index) {
                target.tabId = it.key();
                break;
            }
        }
    } else {
        target.page = current;
    }
    backTargets_.push_back(target);
    pageStack_->setCurrentWidget(page);
    topBar_->setBackVisible(true);
}

void HomeShell::openStationDetail(const charging::model::Station& station, int distanceMeters)
{
    // 路由携带站点快照（含站点 ID）：非法/缺失 ID 由服务详情通道回错误态。
    detailPage_->openStation(station, distanceMeters);
    // 复用全局顶部导航的返回按钮回列表（不重复开发页面级导航）。
    pushRoute(detailPage_);
}

void HomeShell::openReservationConfirm(const charging::model::Station& station,
                                       const charging::model::Charger& charger,
                                       int distanceMeters)
{
    // 独立预约确认页面路由（任务 #17 迭代，替代弹窗）：由详情页在满足
    // 预约条件（已登录 + 无未结束预约）后发信号进入；出发位置（详情页）
    // 由 pushRoute 记入返回栈，返回按钮保持可见。
    confirmPage_->openContext(station, charger, distanceMeters);
    pushRoute(confirmPage_);
}

void HomeShell::openReservationModule()
{
    if (!hasUser_) {
        // 未登录访问“我的预约”：提示登录后跳登录页（沿用全局登录态）。
        showReservationLoginPrompt();
        return;
    }
    modulePage_->refresh();
    // 模块返回目的地固定为“我的”Tab（入口语义，与 develop 迭代前行为一致：
    // 无论经确认页成功路由还是“去查看”拦截进入，返回都回个人中心）。
    backTargets_.clear();
    backTargets_.push_back(BackTarget{nullptr, QStringLiteral("profile")});
    pageStack_->setCurrentWidget(modulePage_);
    topBar_->setBackVisible(true);
}

void HomeShell::openOrderDetail(const charging::client::OrderSummary& summary)
{
    currentSummary_ = summary;
    if (summary.order.status == charging::model::OrderStatus::Charging) {
        // 充电中订单 → 充电过程页：这是唯一的“停止充电”入口（详情页纯展示）。
        charging::client::ChargingStatus live;
        live.order = summary.order;
        live.stationName = summary.stationName;
        live.chargerCode = summary.chargerCode;
        chargingPage_->startFor(live);
        pushRoute(chargingPage_);
        return;
    }
    // 路由携带订单快照：详情页纯展示，结算页也由此上下文进入（不本地估算）。
    orderDetailPage_->showOrder(summary);
    pushRoute(orderDetailPage_);
}

void HomeShell::openSettlement()
{
    // 待支付订单 → 结算页：数据取路由上下文快照（功率置未知：会话已停）。
    charging::client::ChargingStatus stopped;
    stopped.order = currentSummary_.order;
    stopped.stationName = currentSummary_.stationName;
    stopped.chargerCode = currentSummary_.chargerCode;
    settlementPage_->showOrder(stopped);
    settlementPage_->setBalance(lastKnownBalanceCents_);
    pushRoute(settlementPage_);
}

void HomeShell::openRecharge()
{
    rechargePage_->setBalance(lastKnownBalanceCents_);
    rechargePage_->resetForm();
    pushRoute(rechargePage_);
}

void HomeShell::openProfileEdit()
{
    profileEditPage_->refresh();
    pushRoute(profileEditPage_);
}

void HomeShell::leaveRoute()
{
    if (backTargets_.isEmpty()) {
        // 兜底：无路由栈信息时与整合前行为一致（回找站列表）。
        topBar_->setBackVisible(false);
        tabBar_->setCurrentTab(QStringLiteral("station"));
        pageStack_->setCurrentIndex(0);
        return;
    }
    const BackTarget target = backTargets_.takeLast();
    if (!target.tabId.isEmpty()) {
        // 回 Tab 页（路由页不改 Tab 选中，通常 Tab 本就处于选中态）。
        backTargets_.clear();
        topBar_->setBackVisible(false);
        const bool alreadyCurrent = tabBar_->currentTab() == target.tabId;
        tabBar_->setCurrentTab(target.tabId);
        pageStack_->setCurrentIndex(tabIndexById().value(target.tabId, 0));
        if (alreadyCurrent) {
            // tabChanged 不会触发：补做 showTab 的进入即重查。
            showTab(target.tabId);
        }
    } else {
        // 回上层路由页（如结算→详情）：保留返回按钮与剩余栈。
        pageStack_->setCurrentWidget(target.page);
        topBar_->setBackVisible(!backTargets_.isEmpty());
    }
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
    // 全端整合：成员 3 的钱包/订单/充电服务目前仍走共享 mock 通道——
    // 服务端请求分发器尚未实现 GET_USER_INFO/RECHARGE/GET_ORDERS 等命令
    // （TODO(contract)，与负责人对齐后再接同一 ClientConnection）。
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
                               tr("登录后可查看充电订单与结算记录。"),
                               pageStack_);
}

QWidget* HomeShell::createRechargePage()
{
    return makePlaceholderPage(QStringLiteral("💰"), tr("充值"),
                               tr("登录后可查看钱包余额并充值。"),
                               pageStack_);
}

QWidget* HomeShell::createProfilePage()
{
    // 仅未登录时使用：登录态“我的”Tab 已由成员 3 的 ProfilePage 中心页
    // 承接（身份/钱包/订单入口/我的预约/退出登录，测试锚点不变）。
    // 未登录也可点“我的预约”入口：由壳统一拦截并提示登录（任务 #17）。
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


} // namespace charging::client::pages::station
