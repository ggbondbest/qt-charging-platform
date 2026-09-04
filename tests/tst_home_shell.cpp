#include "charging/client/profile_charging/charging_page.h"
#include "charging/client/profile_charging/order_detail_page.h"
#include "charging/client/profile_charging/order_list_page.h"
#include "charging/client/profile_charging/recharge_page.h"
#include "charging/client/profile_charging/settlement_page.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/top_nav_bar.h"
#include "pages/station/home_shell.h"
#include "pages/station/reservation_completed_page.h"
#include "pages/station/reservation_confirm_page.h"
#include "pages/station/reservation_module_page.h"
#include "pages/station/reservation_order_page.h"
#include "pages/station/station_detail_page.h"
#include "pages/station/station_home_page.h"
#include "services/reservation/reservation_service.h"

#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QtTest>

namespace {

using HomeShell = charging::client::pages::station::HomeShell;
using ReservationCompletedPage = charging::client::pages::station::ReservationCompletedPage;
using ReservationConfirmPage = charging::client::pages::station::ReservationConfirmPage;
using ReservationModulePage = charging::client::pages::station::ReservationModulePage;
using ReservationOrderPage = charging::client::pages::station::ReservationOrderPage;
using StationDetailPage = charging::client::pages::station::StationDetailPage;
using StationHomePage = charging::client::pages::station::StationHomePage;
using ReservationService = charging::client::services::reservation::ReservationService;
using ReservationRecord = charging::client::services::reservation::ReservationRecord;

// 供 UI 评审使用的样例用户（新注册用户：昵称 用户+后四位，余额 123.45 元）。
charging::model::User makeSampleUser()
{
    charging::model::User user;
    user.id = 42;
    user.phone = QStringLiteral("13912345678");
    user.nickname = QStringLiteral("用户5678");
    user.balanceCents = 12345;
    return user;
}

// 设置 CHARGING_SNAPSHOT_DIR 时保存页面截图，用于 PR 的 UI 评审证据。
void saveSnapshotIfRequested(QWidget& widget, const QString& fileName)
{
    const QByteArray snapshotDir = qgetenv("CHARGING_SNAPSHOT_DIR");
    if (snapshotDir.isEmpty()) {
        return;
    }
    const QString directory = QString::fromLocal8Bit(snapshotDir);
    QDir().mkpath(directory);
    widget.grab().save(directory + QStringLiteral("/") + fileName);
}

QPushButton* tabButton(QWidget& shell, const QString& id)
{
    return shell.findChild<QPushButton*>(QStringLiteral("tab_") + id);
}

// 等待初始模拟查询完成（模拟通道带延迟，用于驱动加载状态）。
void waitForStationList(HomeShell& shell)
{
    auto* page = shell.findChild<StationHomePage*>();
    QVERIFY(page != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(page->viewState() == StationHomePage::ViewState::List, 3000);
}

// 构造仅带 ID 的站点快照：详情通道会以数据源回查覆盖状态字段。
charging::model::Station makeStationSnapshot(qint64 id,
                                             charging::model::StationStatus status
                                                 = charging::model::StationStatus::Active)
{
    charging::model::Station station;
    station.id = id;
    station.name = QStringLiteral("测试站点 %1").arg(id);
    station.address = QStringLiteral("测试地址 %1").arg(id);
    station.priceCentsPerKwh = 100;
    station.status = status;
    return station;
}

StationDetailPage* detailPage(HomeShell& shell)
{
    auto* detail = shell.findChild<StationDetailPage*>();
    Q_ASSERT_X(detail != nullptr, "detailPage", "HomeShell must own a StationDetailPage");
    return detail;
}

ReservationConfirmPage* confirmPage(HomeShell& shell)
{
    auto* page = shell.findChild<ReservationConfirmPage*>();
    Q_ASSERT_X(page != nullptr, "confirmPage", "HomeShell must own a ReservationConfirmPage");
    return page;
}

ReservationModulePage* modulePage(HomeShell& shell)
{
    auto* page = shell.reservationModule();
    Q_ASSERT_X(page != nullptr, "modulePage", "HomeShell must own a ReservationModulePage");
    return page;
}

// 非拥有：预约服务经模块页访问器取回（同一实例贯穿详情/确认/模块）。
ReservationService* reservationService(HomeShell& shell)
{
    auto* service = modulePage(shell)->service();
    Q_ASSERT_X(service != nullptr, "reservationService", "module must hold the injected service");
    return service;
}

// 详情页当前全部“预约”按钮（每张充电桩卡一个，非空闲置灰）。
QList<QPushButton*> reserveButtons(StationDetailPage& detail)
{
    QList<QPushButton*> buttons;
    const auto all = detail.findChildren<QPushButton*>();
    for (auto* button : all) {
        if (button->objectName() == QStringLiteral("detailReserveButton")) {
            buttons.append(button);
        }
    }
    return buttons;
}

int enabledCount(const QList<QPushButton*>& buttons)
{
    int count = 0;
    for (const auto* button : buttons) {
        count += button->isEnabled() ? 1 : 0;
    }
    return count;
}

QPushButton* firstEnabledReserveButton(StationDetailPage& detail)
{
    for (auto* button : reserveButtons(detail)) {
        if (button->isEnabled()) {
            return button;
        }
    }
    return nullptr;
}

// 打开站点详情并等待桩列表就绪（页面切到详情路由）。
void openDetailAndWait(HomeShell& shell, qint64 stationId, int distanceMeters)
{
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* detail = detailPage(shell);
    detail->openStation(makeStationSnapshot(stationId), distanceMeters);
    pageStack->setCurrentWidget(detail);
    QTest::qWait(20);
    QVERIFY(detail->viewState() == StationDetailPage::DetailState::Loading
            || detail->viewState() == StationDetailPage::DetailState::Ready);
    QTRY_VERIFY_WITH_TIMEOUT(detail->viewState() == StationDetailPage::DetailState::Ready, 3000);
}

// 从“我的”Tab 入口进入“我的预约”模块路由页。
void openModuleViaProfileTab(HomeShell& shell)
{
    tabButton(shell, QStringLiteral("profile"))->click();
    auto* entry = shell.findChild<QPushButton*>(QStringLiteral("openReservationsButton"));
    QVERIFY(entry != nullptr);
    entry->click();
}

// 清空模拟预约记录：解除“仅一条未结束预约”约束，便于演示成功路径。
void clearReservations(HomeShell& shell)
{
    reservationService(shell)->setMockRecords({});
}

// 点击空闲桩预约按钮，断言路由至独立预约确认页面（满足条件路径）。
void clickReserveAndWaitConfirm(HomeShell& shell)
{
    auto* detail = detailPage(shell);
    auto* enabledButton = firstEnabledReserveButton(*detail);
    QVERIFY(enabledButton != nullptr);
    enabledButton->click();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    QCOMPARE(pageStack->currentIndex(), 5);
}

} // namespace

class HomeShellTest final : public QObject
{
    Q_OBJECT

private slots:
    // —— 任务 #2：导航外壳 ——
    void loggedInShellRendersTopBarWithUser();
    void loggedOutShellShowsLoginButtonAndEmits();
    void startsOnStationTab();
    void togglesBetweenTabs();
    void avatarOpensProfilePage();
    void profilePageRedesignKeepsUserAndAddsFunctionSlots();
    // —— 任务 #7：找站业务 ——
    void initialSearchGoesThroughLoadingToResultList();
    void topBarSearchFiltersStationList();
    void noMatchShowsEmptyState();
    void errorStateOffersFriendlyRetry();
    void sortAndPriceFiltersRefreshInstantly();
    void cardClickOpensDetailRouteAndBackReturns();
    // —— 任务 #12：站点详情业务 ——
    void detailPageShowsChargersWithFaultAndReservation();
    void detailEmptyAndOfflineStates();
    void detailInvalidRouteShowsErrorAndBackHome();
    // —— 任务 #17 迭代：预约确认页面 / 单预约约束 / 预约模块 ——
    void confirmPageOpensWithReservationContext();
    void confirmCloseReturnsToDetail();
    void confirmSubmitRoutesToReservationOrderTab();
    void confirmFailureKeepsPageOpenForRetry();
    void unfinishedReservationBlocksNewConfirmWithPrompt();
    void reservationWithoutLoginPromptsAndRoutesToLogin();
    void reservationModuleShowsOrderAndCompletedSubTabs();
    void cancelReservationSwitchesToCompletedTab();
    void reservationEmptyAndErrorStates();
    void countdownThresholdsAndExpiryTransition();
    void moduleRouteBackReturnsToProfile();
    // —— 全端整合回归：充电中订单路由、支付完成回订单 Tab、充值回跳、空态布局 ——
    void chargingOrderRoutesToChargingPage();
    void settlementDoneReturnsToFilteredOrderTab();
    void rechargeFromSettlementReturnsAndUnlocksPay();
    void emptyOrderListNoticeFillsListArea();
};

void HomeShellTest::loggedInShellRendersTopBarWithUser()
{
    HomeShell shell(makeSampleUser());
    shell.show();

    auto* topBar = shell.findChild<charging::client::TopNavBar*>();
    QVERIFY(topBar != nullptr);
    QVERIFY(topBar->hasUser());

    auto* loginButton = shell.findChild<QPushButton*>(QStringLiteral("navLoginButton"));
    auto* avatarButton = shell.findChild<QPushButton*>(QStringLiteral("navAvatarButton"));
    auto* searchLineEdit = shell.findChild<QLineEdit*>(QStringLiteral("navSearchLineEdit"));
    QVERIFY(loginButton != nullptr);
    QVERIFY(avatarButton != nullptr);
    QVERIFY(searchLineEdit != nullptr);
    // 已登录：右侧显示头像、隐藏登录按钮；搜索框在中间。
    QVERIFY(loginButton->isHidden());
    QVERIFY(!avatarButton->isHidden());

    waitForStationList(shell);
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_station.png"));
}

void HomeShellTest::loggedOutShellShowsLoginButtonAndEmits()
{
    // 未登录进入首页：右上角必须显示登录按钮（规格异常路径）。
    HomeShell shell;

    auto* topBar = shell.findChild<charging::client::TopNavBar*>();
    QVERIFY(topBar != nullptr);
    QVERIFY(!topBar->hasUser());

    QSignalSpy loginSpy(&shell, &HomeShell::loginRequested);
    auto* loginButton = shell.findChild<QPushButton*>(QStringLiteral("navLoginButton"));
    auto* avatarButton = shell.findChild<QPushButton*>(QStringLiteral("navAvatarButton"));
    QVERIFY(loginButton != nullptr);
    QVERIFY(!loginButton->isHidden());
    QVERIFY(avatarButton->isHidden());

    loginButton->click();
    QCOMPARE(loginSpy.count(), 1);
}

void HomeShellTest::startsOnStationTab()
{
    HomeShell shell(makeSampleUser());

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* stationTab = tabButton(shell, QStringLiteral("station"));
    auto* orderTab = tabButton(shell, QStringLiteral("order"));
    auto* rechargeTab = tabButton(shell, QStringLiteral("recharge"));
    auto* profileTab = tabButton(shell, QStringLiteral("profile"));
    QVERIFY(pageStack != nullptr);
    QVERIFY(stationTab != nullptr);
    QVERIFY(orderTab != nullptr);
    QVERIFY(rechargeTab != nullptr);
    QVERIFY(profileTab != nullptr);
    // 4 个 Tab 页 + 详情/预约确认/预约模块路由页（成员 2）
    // + 成员 3 整合路由页 5 个（订单详情/结算/充值/编辑资料/充电过程）= 12。
    QCOMPARE(pageStack->count(), 12);

    // 登录后默认落在“找站”（首页）。
    QCOMPARE(pageStack->currentIndex(), 0);
    QVERIFY(stationTab->isChecked());
    QVERIFY(!orderTab->isChecked());
    QVERIFY(!rechargeTab->isChecked());
    QVERIFY(!profileTab->isChecked());
}

void HomeShellTest::togglesBetweenTabs()
{
    HomeShell shell(makeSampleUser());
    shell.show();

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* stationTab = tabButton(shell, QStringLiteral("station"));
    auto* orderTab = tabButton(shell, QStringLiteral("order"));
    auto* rechargeTab = tabButton(shell, QStringLiteral("recharge"));
    auto* profileTab = tabButton(shell, QStringLiteral("profile"));
    QVERIFY(pageStack != nullptr);

    struct Expectation
    {
        QPushButton* button;
        int index;
    };

    const QList<Expectation> expectations = {
        {orderTab, 1}, {rechargeTab, 2}, {profileTab, 3}, {stationTab, 0}, {orderTab, 1},
    };
    for (const auto& expectation : expectations) {
        expectation.button->click();
        QCOMPARE(pageStack->currentIndex(), expectation.index);
        QVERIFY(expectation.button->isChecked());
        // 同一时刻只允许一个 Tab 处于选中态。
        const int checkedCount = stationTab->isChecked() + orderTab->isChecked()
            + rechargeTab->isChecked() + profileTab->isChecked();
        QCOMPARE(checkedCount, 1);
    }

    orderTab->click();
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_order.png"));
    rechargeTab->click();
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_recharge.png"));
    profileTab->click();
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_profile.png"));
}

void HomeShellTest::avatarOpensProfilePage()
{
    // 已登录点击顶部头像 → 跳转个人中心（“我的”Tab）。
    HomeShell shell(makeSampleUser());
    shell.show();

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* avatarButton = shell.findChild<QPushButton*>(QStringLiteral("navAvatarButton"));
    QVERIFY(avatarButton != nullptr);

    avatarButton->click();
    QCOMPARE(pageStack->currentIndex(), 3);
    QVERIFY(tabButton(shell, QStringLiteral("profile"))->isChecked());
}

void HomeShellTest::profilePageRedesignKeepsUserAndAddsFunctionSlots()
{
    // 全端整合：登录态“我的”Tab 由成员 3 的 ProfilePage 中心页承接（取代
    // 任务 #17 迭代期的临时个人中心）。身份/余额/预约入口/退出登录锚点
    // 语义保持；占位功能卡片由真实的订单四宫格角标与资料/钱包入口取代。
    HomeShell shell(makeSampleUser());

    QSignalSpy logoutSpy(&shell, &HomeShell::logoutRequested);
    tabButton(shell, QStringLiteral("profile"))->click();

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    QCOMPARE(pageStack->currentIndex(), 3);

    auto* nicknameLabel = shell.findChild<QLabel*>(QStringLiteral("nicknameLabel"));
    auto* balanceLabel = shell.findChild<QLabel*>(QStringLiteral("balanceLabel"));
    QVERIFY(nicknameLabel != nullptr);
    QVERIFY(balanceLabel != nullptr);
    QVERIFY(nicknameLabel->text().contains(QStringLiteral("用户5678")));
    QVERIFY(balanceLabel->text().contains(QStringLiteral("123.45")));

    // “我的预约”入口保留测试锚点，点击经壳路由至预约模块。
    auto* reservationsEntry =
        shell.findChild<QPushButton*>(QStringLiteral("openReservationsButton"));
    QVERIFY(reservationsEntry != nullptr);

    // 退出登录：红色危险文案（全局 QSS #logoutButton），点击发 logoutRequested。
    auto* logoutButton = shell.findChild<QPushButton*>(QStringLiteral("logoutButton"));
    QVERIFY(logoutButton != nullptr);
    QCOMPARE(logoutButton->text(), QStringLiteral("退出登录"));
    logoutButton->click();
    QCOMPARE(logoutSpy.count(), 1);
}

void HomeShellTest::initialSearchGoesThroughLoadingToResultList()
{
    // 进入页面即发起检索：先加载态，模拟数据返回后展示列表。
    HomeShell shell(makeSampleUser());
    shell.show();

    auto* page = shell.findChild<StationHomePage*>();
    QVERIFY(page != nullptr);
    QCOMPARE(page->viewState(), StationHomePage::ViewState::Loading);

    QTRY_VERIFY_WITH_TIMEOUT(page->viewState() == StationHomePage::ViewState::List, 3000);
    // 模拟数据共 6 个站点（含离线站与无桩站，驱动详情页边界状态演示）。
    QCOMPARE(page->stationCardCount(), 6);
}

void HomeShellTest::topBarSearchFiltersStationList()
{
    // 地址搜索走顶部导航公共组件的搜索框，不再另建输入框。
    HomeShell shell(makeSampleUser());
    shell.show();
    waitForStationList(shell);

    auto* page = shell.findChild<StationHomePage*>();
    auto* searchLineEdit = shell.findChild<QLineEdit*>(QStringLiteral("navSearchLineEdit"));
    QVERIFY(searchLineEdit != nullptr);

    searchLineEdit->setText(QStringLiteral("科技园"));
    QTest::keyClick(searchLineEdit, Qt::Key_Return);

    // 触发检索 → 加载态 → 命中 1 条。
    QCOMPARE(page->viewState(), StationHomePage::ViewState::Loading);
    QTRY_VERIFY_WITH_TIMEOUT(page->viewState() == StationHomePage::ViewState::List, 3000);
    QCOMPARE(page->stationCardCount(), 1);
    QCOMPARE(page->currentKeyword(), QStringLiteral("科技园"));
}

void HomeShellTest::noMatchShowsEmptyState()
{
    HomeShell shell(makeSampleUser());
    shell.show();
    waitForStationList(shell);

    auto* page = shell.findChild<StationHomePage*>();
    page->search(QStringLiteral("不存在的地方"));
    QTRY_VERIFY_WITH_TIMEOUT(page->viewState() == StationHomePage::ViewState::Empty, 3000);
}

void HomeShellTest::errorStateOffersFriendlyRetry()
{
    // 异常分支：服务报错时列表区展示错误提示；重试成功后回到列表。
    HomeShell shell(makeSampleUser());
    shell.show();
    waitForStationList(shell);

    auto* page = shell.findChild<StationHomePage*>();
    page->service()->setSimulateFailure(true);
    page->search(QStringLiteral("充电"));
    QTRY_VERIFY_WITH_TIMEOUT(page->viewState() == StationHomePage::ViewState::Error, 3000);

    page->service()->setSimulateFailure(false);
    QMetaObject::invokeMethod(page, "retrySearch");
    QTRY_VERIFY_WITH_TIMEOUT(page->viewState() == StationHomePage::ViewState::List, 3000);
    QVERIFY(page->stationCardCount() > 0);
}

void HomeShellTest::sortAndPriceFiltersRefreshInstantly()
{
    HomeShell shell(makeSampleUser());
    shell.show();
    waitForStationList(shell);

    auto* page = shell.findChild<StationHomePage*>();
    auto* sortAvailable = shell.findChild<QPushButton*>(QStringLiteral("sortAvailableButton"));
    auto* sortDistance = shell.findChild<QPushButton*>(QStringLiteral("sortDistanceButton"));
    auto* priceCombo = shell.findChild<QComboBox*>(QStringLiteral("priceFilterComboBox"));
    QVERIFY(sortAvailable != nullptr);
    QVERIFY(sortDistance != nullptr);
    QVERIFY(priceCombo != nullptr);

    // 空闲优先：available 最高的是后海城市广场站（id 5，空 7/12）。
    sortAvailable->click();
    QCOMPARE(page->viewState(), StationHomePage::ViewState::List); // 本地投影：即时生效
    QCOMPARE(page->visibleStationIds().constFirst(), qint64(5));

    // 距离最近：第一张卡应为科技园充电驿站（id 1，850m）。
    sortDistance->click();
    QCOMPARE(page->viewState(), StationHomePage::ViewState::List);
    QCOMPARE(page->visibleStationIds().constFirst(), qint64(1));

    // 电价筛选 ≤ ¥1.00：模拟数据中 98/86/92 三条命中。
    priceCombo->setCurrentIndex(1);
    QCOMPARE(page->stationCardCount(), 3);

    priceCombo->setCurrentIndex(0); // 全部电价
    QCOMPARE(page->stationCardCount(), 6);
}

void HomeShellTest::cardClickOpensDetailRouteAndBackReturns()
{
    // 站点卡片点击 → 详情路由页（任务 #12）；顶部导航“返回”回找站列表。
    HomeShell shell(makeSampleUser());
    shell.show();
    QTest::qWait(20); // 让窗口完成映射，鼠标事件落在真实几何上。
    waitForStationList(shell);

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* page = shell.findChild<StationHomePage*>();
    auto* topBar = shell.findChild<charging::client::TopNavBar*>();
    QVERIFY(topBar != nullptr);
    auto* card = page->stationCardAt(0);
    QVERIFY(card != nullptr);

    QTest::mouseClick(card, Qt::LeftButton);
    QCOMPARE(pageStack->currentIndex(), 4);

    // 路由携带站点快照：信息区立即可见，桩列表经加载态后就绪。
    auto* detail = detailPage(shell);
    auto* nameLabel = shell.findChild<QLabel*>(QStringLiteral("detailNameLabel"));
    QVERIFY(nameLabel != nullptr);
    QVERIFY(!nameLabel->text().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(detail->viewState() == StationDetailPage::DetailState::Ready, 3000);

    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_detail.png"));

    // 返回按钮复用全局顶部导航（进入详情显示、返回列表收起）。
    auto* backButton = shell.findChild<QPushButton*>(QStringLiteral("navBackButton"));
    QVERIFY(backButton != nullptr);
    QVERIFY(backButton->isVisible());
    backButton->click();
    QCOMPARE(pageStack->currentIndex(), 0);
    QVERIFY(!topBar->isBackVisible());

    // 详情页内点击“找站”Tab 也应能回列表（重复点击当前 Tab 不被去重吞掉）。
    QTest::mouseClick(page->stationCardAt(0), Qt::LeftButton);
    QCOMPARE(pageStack->currentIndex(), 4);
    QVERIFY(backButton->isVisible());
    tabButton(shell, QStringLiteral("station"))->click();
    QCOMPARE(pageStack->currentIndex(), 0);
    QVERIFY(!topBar->isBackVisible());
}

void HomeShellTest::detailPageShowsChargersWithFaultAndReservation()
{
    // 正常态：充电桩卡片列表 + 故障视觉标记 + 预约入口按钮。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* detail = detailPage(shell);

    detail->openStation(makeStationSnapshot(1), 850);
    QCOMPARE(detail->viewState(), StationDetailPage::DetailState::Loading);
    QTRY_VERIFY_WITH_TIMEOUT(detail->viewState() == StationDetailPage::DetailState::Ready, 3000);
    pageStack->setCurrentIndex(4); // 让详情页成为当前页（截图取证）。
    QTest::qWait(20);
    // id1：10 桩，空闲 3/共 10（与列表页空位数一致）。
    QCOMPARE(detail->chargerCardCount(), 10);
    auto* summary = shell.findChild<QLabel*>(QStringLiteral("detailChargerSummaryLabel"));
    QVERIFY(summary != nullptr);
    QVERIFY(summary->text().contains(QStringLiteral("空闲 3")));

    // 故障桩卡片带红色标记属性（页面局部样式驱动视觉）。
    bool sawFaultCard = false;
    const auto frames = detail->findChildren<QFrame*>();
    for (const auto* frame : frames) {
        if (frame->property("isChargerCard").toBool()
            && frame->property("chargerFault").toBool()) {
            sawFaultCard = true;
            break;
        }
    }
    QVERIFY(sawFaultCard);

    // 预约入口（任务 #17）：所有桩都有按钮，仅空闲可点；非空闲置灰。
    const auto buttons = reserveButtons(*detail);
    QCOMPARE(buttons.size(), 10);
    QCOMPARE(enabledCount(buttons), 3); // id1：空闲 3
    for (const auto* button : buttons) {
        if (!button->isEnabled()) {
            QVERIFY(!button->toolTip().isEmpty()); // 置灰需说明原因
        }
    }

    QSignalSpy reservationSpy(detail, &StationDetailPage::reservationRequested);
    QPushButton* disabledButton = nullptr;
    QPushButton* enabledButton = nullptr;
    for (auto* button : buttons) {
        if (button->isEnabled() && enabledButton == nullptr) {
            enabledButton = button;
        } else if (!button->isEnabled() && disabledButton == nullptr) {
            disabledButton = button;
        }
    }
    QVERIFY(disabledButton != nullptr);
    disabledButton->click(); // 置灰不可点击：不触发任何交互
    QCOMPARE(reservationSpy.count(), 0);

    // 空闲桩点击 → 发出预约请求信号（默认模拟数据已有未结束预约，
    // 路由拦截行为由 unfinishedReservationBlocksNewConfirmWithPrompt 覆盖）。
    QVERIFY(enabledButton != nullptr);
    enabledButton->click();
    QCOMPARE(reservationSpy.count(), 1);
    // 未跳转确认页（仍停留在详情页路由）。
    QCOMPARE(pageStack->currentIndex(), 4);

    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_detail_chargers.png"));
}

void HomeShellTest::detailEmptyAndOfflineStates()
{
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* detail = detailPage(shell);

    // 空数据：无桩站点 → 列表区“暂无充电桩”，页面不留大片空白。
    detail->openStation(makeStationSnapshot(6), 3800);
    QTRY_VERIFY_WITH_TIMEOUT(detail->viewState() == StationDetailPage::DetailState::Ready, 3000);
    QCOMPARE(detail->chargerCardCount(), 0);
    QVERIFY(detail->chargerEmptyVisible());
    pageStack->setCurrentIndex(4);
    QTest::qWait(20); // 等待重绘，截图作为 UI 评审证据。
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_detail_empty.png"));

    // 站点离线：信息状态 + 醒目横幅（数据源驱动）。
    detail->openStation(makeStationSnapshot(4), 2650);
    QTRY_VERIFY_WITH_TIMEOUT(detail->viewState() == StationDetailPage::DetailState::Ready, 3000);
    QVERIFY(detail->offlineBannerVisible());
    auto* statusTag = shell.findChild<QLabel*>(QStringLiteral("detailStatusTag"));
    QVERIFY(statusTag != nullptr);
    QVERIFY(statusTag->text().contains(QStringLiteral("已离线")));
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_detail_offline.png"));
}

void HomeShellTest::detailInvalidRouteShowsErrorAndBackHome()
{
    // 无站点 ID / ID 非法：错误提示 + “返回首页”回到找站列表。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* detail = detailPage(shell);

    detail->openStation(makeStationSnapshot(0), 0);
    QTRY_VERIFY_WITH_TIMEOUT(detail->viewState() == StationDetailPage::DetailState::Error, 3000);

    QPushButton* backHomeButton = nullptr;
    const auto buttons = detail->findChildren<QPushButton*>();
    for (auto* button : buttons) {
        if (button->text() == QStringLiteral("返回首页")) {
            backHomeButton = button;
            break;
        }
    }
    QVERIFY(backHomeButton != nullptr);
    backHomeButton->click();
    QCOMPARE(pageStack->currentIndex(), 0);
}

void HomeShellTest::confirmPageOpensWithReservationContext()
{
    // 任务 #17 迭代：满足预约条件（已登录 + 无未结束预约）→ 路由至独立
    // 预约确认页面，展示站点/桩编号/充电类型功率/时长下拉/预估费用。
    HomeShell shell(makeSampleUser());
    shell.show();
    clearReservations(shell);
    openDetailAndWait(shell, 1, 850);
    clickReserveAndWaitConfirm(shell);

    auto* page = confirmPage(shell);
    auto* stationLabel = shell.findChild<QLabel*>(QStringLiteral("confirmStationNameLabel"));
    auto* chargerLabel = shell.findChild<QLabel*>(QStringLiteral("confirmChargerCodeLabel"));
    auto* specLabel = shell.findChild<QLabel*>(QStringLiteral("confirmChargerSpecLabel"));
    QVERIFY(stationLabel != nullptr && stationLabel->isVisible());
    QVERIFY(chargerLabel != nullptr && !chargerLabel->text().isEmpty());
    QVERIFY(specLabel != nullptr && specLabel->text().contains(QStringLiteral("kW")));
    QVERIFY(page->pageState() == ReservationConfirmPage::PageState::Idle);

    // 时长下拉默认 60 分钟；预估费用 = 电价 × 时长（id1 电价 120 分/度）。
    QCOMPARE(page->selectedMinutes(), 60);
    QVERIFY(page->estimatedFeeText().contains(QStringLiteral("¥1.20")));
    auto* combo = shell.findChild<QComboBox*>(QStringLiteral("reservationDurationComboBox"));
    QVERIFY(combo != nullptr);
    combo->setCurrentIndex(2); // 90 分钟联动刷新
    QCOMPARE(page->selectedMinutes(), 90);
    QVERIFY(page->estimatedFeeText().contains(QStringLiteral("¥1.80")));

    // 顶部导航“返回”→ 回站点详情页（不重复开发导航）。
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    shell.findChild<QPushButton*>(QStringLiteral("navBackButton"))->click();
    QCOMPARE(pageStack->currentIndex(), 4);
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_confirm.png"));
}

void HomeShellTest::confirmCloseReturnsToDetail()
{
    // 【关闭】按钮 → 返回站点详情页。
    HomeShell shell(makeSampleUser());
    shell.show();
    clearReservations(shell);
    openDetailAndWait(shell, 1, 850);
    clickReserveAndWaitConfirm(shell);

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    shell.findChild<QPushButton*>(QStringLiteral("reservationCloseButton"))->click();
    QCOMPARE(pageStack->currentIndex(), 4);
}

void HomeShellTest::confirmSubmitRoutesToReservationOrderTab()
{
    // 确认预约成功：loading 提交态 → 自动跳转【预约订单】页（模块二级 Tab）。
    HomeShell shell(makeSampleUser());
    shell.show();
    clearReservations(shell);
    openDetailAndWait(shell, 1, 850);
    clickReserveAndWaitConfirm(shell);

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* confirmButton = shell.findChild<QPushButton*>(QStringLiteral("reservationConfirmButton"));
    QVERIFY(confirmButton != nullptr);
    confirmButton->click();
    // 提交中：按钮禁用防重复提交（loading 态）。
    QVERIFY(!confirmButton->isEnabled());
    QVERIFY(confirmButton->text().contains(QStringLiteral("提交中")));

    QTRY_VERIFY_WITH_TIMEOUT(pageStack->currentIndex() == 6, 5000);
    auto* module = modulePage(shell);
    QCOMPARE(module->currentSubTab(), QStringLiteral("order"));
    auto* order = module->orderPage();
    QTRY_VERIFY_WITH_TIMEOUT(
        order->viewState() == ReservationOrderPage::PageState::Active, 5000);
    QVERIFY(!order->countdownText().isEmpty());
    // 单预约约束：成功预约后存在未结束预约，二次预约将被入口拦截。
    QVERIFY(module->service()->hasUnfinishedReservation());

    // 确认页复位（下次进入为 Idle 可编辑态）。
    QCOMPARE(confirmPage(shell)->pageState(), ReservationConfirmPage::PageState::Idle);
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_order_tab.png"));
}

void HomeShellTest::confirmFailureKeepsPageOpenForRetry()
{
    // 提交失败（桩被抢占）：红色原因展示、停留在本页、可修改后重试。
    HomeShell shell(makeSampleUser());
    shell.show();
    clearReservations(shell);
    openDetailAndWait(shell, 1, 850);
    clickReserveAndWaitConfirm(shell);

    reservationService(shell)->setSimulateNextSubmitConflict(true);
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* confirmButton = shell.findChild<QPushButton*>(QStringLiteral("reservationConfirmButton"));
    confirmButton->click();

    QTRY_VERIFY_WITH_TIMEOUT(confirmPage(shell)->messageText().contains(QStringLiteral("抢占")),
                             5000);
    QCOMPARE(pageStack->currentIndex(), 5); // 失败不跳转
    QVERIFY(confirmButton->isEnabled());    // 恢复可再次尝试
    QVERIFY(confirmButton->text().contains(QStringLiteral("确认预约")));
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_confirm_failed.png"));

    // 关闭仍可返回详情页。
    shell.findChild<QPushButton*>(QStringLiteral("reservationCloseButton"))->click();
    QCOMPARE(pageStack->currentIndex(), 4);
}

void HomeShellTest::unfinishedReservationBlocksNewConfirmWithPrompt()
{
    // 业务约束：存在未结束预约时点“预约”→ 提示拦截、不跳转；
    // “去查看”直达【预约订单】页。
    HomeShell shell(makeSampleUser());
    shell.show();
    openDetailAndWait(shell, 1, 850);
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));

    QVERIFY(firstEnabledReserveButton(*detailPage(shell)) != nullptr);
    firstEnabledReserveButton(*detailPage(shell))->click();

    auto* prompt = shell.findChild<QMessageBox*>(QStringLiteral("unfinishedReservationPrompt"));
    QVERIFY(prompt != nullptr);
    QVERIFY(prompt->isVisible());
    // 提示文案与规格逐字一致。
    QCOMPARE(prompt->text(),
             QStringLiteral("您当前尚有未结束的预约，请结束当前预约后再发起新预约"));
    QCOMPARE(pageStack->currentIndex(), 4); // 不跳转确认页

    auto* goLook = prompt->findChild<QPushButton*>(QStringLiteral("unfinishedGoLookButton"));
    QVERIFY(goLook != nullptr);
    goLook->click();
    QCOMPARE(pageStack->currentIndex(), 6); // 直达我的预约模块
    QCOMPARE(modulePage(shell)->currentSubTab(), QStringLiteral("order"));
    QTRY_VERIFY_WITH_TIMEOUT(
        shell.findChild<QMessageBox*>(QStringLiteral("unfinishedReservationPrompt")) == nullptr,
        3000);

    // “知道了”分支：仅关闭提示，留在详情页。
    firstEnabledReserveButton(*detailPage(shell))->click();
    QCOMPARE(pageStack->currentIndex(), 6); // 仍在模块（提示不改变路由）
    auto* prompt2 = shell.findChild<QMessageBox*>(QStringLiteral("unfinishedReservationPrompt"));
    QVERIFY(prompt2 != nullptr);
    auto dismissIt = [prompt2]() {
        const auto buttons = prompt2->findChildren<QPushButton*>();
        for (auto* button : buttons) {
            if (button->text() == QStringLiteral("知道了")) {
                return button;
            }
        }
        return static_cast<QPushButton*>(nullptr);
    };
    QPushButton* known = dismissIt();
    QVERIFY(known != nullptr);
    known->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        shell.findChild<QMessageBox*>(QStringLiteral("unfinishedReservationPrompt")) == nullptr,
        3000);
    QCOMPARE(pageStack->currentIndex(), 6);
}

void HomeShellTest::reservationWithoutLoginPromptsAndRoutesToLogin()
{
    // 未登录点击预约：提示登录，“去登录”经全局 loginRequested 跳登录页。
    HomeShell shell;
    shell.show();
    openDetailAndWait(shell, 1, 850);
    auto* detail = detailPage(shell);

    QSignalSpy loginSpy(&shell, &HomeShell::loginRequested);
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* enabledButton = firstEnabledReserveButton(*detail);
    QVERIFY(enabledButton != nullptr);
    enabledButton->click();

    auto* prompt = shell.findChild<QMessageBox*>(QStringLiteral("reservationLoginPrompt"));
    QVERIFY(prompt != nullptr);
    QVERIFY(prompt->isVisible());
    // 未登录不进入预约确认页（仍停留在详情路由）。
    QCOMPARE(pageStack->currentIndex(), 4);

    auto* goLogin = prompt->findChild<QPushButton*>(QStringLiteral("reservationGoLoginButton"));
    QVERIFY(goLogin != nullptr);
    goLogin->click();
    QCOMPARE(loginSpy.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        shell.findChild<QMessageBox*>(QStringLiteral("reservationLoginPrompt")) == nullptr, 3000);
}

void HomeShellTest::reservationModuleShowsOrderAndCompletedSubTabs()
{
    // 预约模块：二级 Tab 切换【预约订单】（三栏）/【已完成的预约】（归档）。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* topBar = shell.findChild<charging::client::TopNavBar*>();

    openModuleViaProfileTab(shell);
    QCOMPARE(pageStack->currentIndex(), 6);
    QVERIFY(topBar->isBackVisible());

    auto* module = modulePage(shell);
    QCOMPARE(module->currentSubTab(), QStringLiteral("order"));

    // 【预约订单】三栏：距离（虚拟占位）/ 倒计时 / 电量（虚拟占位）。
    auto* order = module->orderPage();
    QTRY_VERIFY_WITH_TIMEOUT(order->viewState() == ReservationOrderPage::PageState::Active, 3000);
    QVERIFY(order->distanceText().contains(QStringLiteral("km"))); // 默认 1250m → 约 1.3 km
    QVERIFY(order->batteryText().contains(QStringLiteral("SOC")));
    QString orderText;
    for (const auto* label : order->findChildren<QLabel*>()) {
        orderText += label->text();
    }
    QVERIFY(orderText.contains(QStringLiteral("南山智造充电站")));
    QVERIFY(orderText.contains(QStringLiteral("SZ-NSZ-03-07")));
    QVERIFY(orderText.contains(QStringLiteral("120kW")));
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_order_tab.png"));

    // 【已完成的预约】：历史卡片（已完成/已取消/已过期）。
    shell.findChild<QPushButton*>(QStringLiteral("reservationHistoryTabButton"))->click();
    QCOMPARE(module->currentSubTab(), QStringLiteral("completed"));
    auto* completed = module->completedPage();
    QTRY_VERIFY_WITH_TIMEOUT(
        completed->viewState() == ReservationCompletedPage::PageState::List, 3000);
    QCOMPARE(completed->recordCardCount(), 3); // 9001 进行中 → 订单页，不进归档
    QString historyText;
    for (const auto* label : completed->findChildren<QLabel*>()) {
        historyText += label->text();
    }
    QVERIFY(historyText.contains(QStringLiteral("已完成")));
    QVERIFY(historyText.contains(QStringLiteral("已取消")));
    QVERIFY(historyText.contains(QStringLiteral("已过期")));
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_completed_tab.png"));

    // 点击历史卡片 → 详情弹窗（全部字段）→ 可关闭。
    QWidget* firstCard = nullptr;
    const auto cards = completed->findChildren<QFrame*>();
    for (auto* frame : cards) {
        if (frame->property("isHistoryReservationCard").toBool()) {
            firstCard = frame;
            break;
        }
    }
    QVERIFY(firstCard != nullptr);
    QTest::mouseClick(firstCard, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(completed->detailDialogVisible(), 3000);
    QVERIFY(completed->detailDialogText().contains(QStringLiteral("站点名称：")));
    QVERIFY(completed->detailDialogText().contains(QStringLiteral("充电桩：")));
    QVERIFY(completed->detailDialogText().contains(QStringLiteral("预约时长：")));
    shell.findChild<QPushButton*>(QStringLiteral("reservationDetailCloseButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!completed->detailDialogVisible(), 3000);

    // 二级 Tab 仅模块内生效：不改变全局底部 Tab 选中。
    QVERIFY(tabButton(shell, QStringLiteral("profile"))->isChecked());
}

void HomeShellTest::cancelReservationSwitchesToCompletedTab()
{
    // 取消预约：成功 → 自动跳转【已完成的预约】页。
    HomeShell shell(makeSampleUser());
    shell.show();
    openModuleViaProfileTab(shell);

    auto* module = modulePage(shell);
    auto* order = module->orderPage();
    QTRY_VERIFY_WITH_TIMEOUT(order->viewState() == ReservationOrderPage::PageState::Active, 3000);

    auto* cancelButton = shell.findChild<QPushButton*>(QStringLiteral("reservationOrderCancelButton"));
    QVERIFY(cancelButton != nullptr);
    cancelButton->click();
    // 取消中：按钮禁用防重复提交（loading 态）。
    QVERIFY(!cancelButton->isEnabled());
    QVERIFY(cancelButton->text().contains(QStringLiteral("取消中")));

    QTRY_VERIFY_WITH_TIMEOUT(module->currentSubTab() == QStringLiteral("completed"), 5000);
    auto* completed = module->completedPage();
    QTRY_VERIFY_WITH_TIMEOUT(
        completed->viewState() == ReservationCompletedPage::PageState::List
            && completed->recordCardCount() == 4,
        5000);

    // 订单页转空态：无进行中预约，可重新发起新预约。
    shell.findChild<QPushButton*>(QStringLiteral("reservationOrderTabButton"))->click();
    QCOMPARE(order->viewState(), ReservationOrderPage::PageState::Empty);
    QVERIFY(!module->service()->hasUnfinishedReservation());
}

void HomeShellTest::reservationEmptyAndErrorStates()
{
    // 空记录 → 订单页友好空态（“去找桩”回找站 Tab）；
    // 接口异常 → 两页错误态 + 重试恢复。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* service = reservationService(shell);
    service->setMockRecords({});

    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    openModuleViaProfileTab(shell);
    auto* module = modulePage(shell);
    auto* order = module->orderPage();
    QTRY_VERIFY_WITH_TIMEOUT(order->viewState() == ReservationOrderPage::PageState::Empty, 3000);
    QVERIFY(order->countdownText().isEmpty()); // 无倒计时残留

    // 订单页空态引导：去找桩 → 回“找站”Tab（路由落回栈顶）。
    QPushButton* findStation = nullptr;
    const auto orderButtons = order->findChildren<QPushButton*>();
    for (auto* button : orderButtons) {
        if (button->text().contains(QStringLiteral("去找桩"))) {
            findStation = button;
            break;
        }
    }
    QVERIFY(findStation != nullptr);
    findStation->click();
    QCOMPARE(pageStack->currentIndex(), 0);
    QVERIFY(tabButton(shell, QStringLiteral("station"))->isChecked());
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_order_empty.png"));

    // 错误态：接口异常 → 模块两页展示友好错误；重试恢复。
    openModuleViaProfileTab(shell);
    // 先等待进入模块触发的拉取收敛（避免与下面的失败注入请求竞争标志）。
    QTRY_VERIFY_WITH_TIMEOUT(order->viewState() == ReservationOrderPage::PageState::Empty, 3000);
    service->setSimulateFailure(true);
    module->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(
        order->viewState() == ReservationOrderPage::PageState::Error, 3000);
    QCOMPARE(module->completedPage()->viewState(), ReservationCompletedPage::PageState::Error);

    // 已完成页错误态“重试”按钮：重新拉取后恢复空态。
    auto* completed = module->completedPage();
    QPushButton* retryButton = nullptr;
    const auto buttons = completed->findChildren<QPushButton*>();
    for (auto* button : buttons) {
        if (button->text() == QStringLiteral("重试")) {
            retryButton = button;
            break;
        }
    }
    QVERIFY(retryButton != nullptr);
    retryButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        completed->viewState() == ReservationCompletedPage::PageState::Empty, 3000);
}

void HomeShellTest::countdownThresholdsAndExpiryTransition()
{
    // 倒计时每秒刷新，剩余时长分档变色：>30min 绿 / 5~30min 黄 / <5min 红；
    // 归零自动流转“已过期”并刷新展示。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* service = reservationService(shell);
    auto* module = modulePage(shell);
    auto* order = module->orderPage();

    const auto makeActiveRecord = [](qint64 id, int secsToExpire) {
        ReservationRecord record;
        record.reservation.id = id;
        record.reservation.userId = 42;
        record.reservation.chargerId = 7000 + id;
        record.reservation.status = charging::model::ReservationStatus::Active;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        record.reservation.reservedAtUtc = now.addSecs(-60);
        record.reservation.expiresAtUtc = now.addSecs(secsToExpire);
        record.stationName = QStringLiteral("倒计时测试站");
        record.chargerCode = QStringLiteral("TEST-01");
        record.chargerSpec = QStringLiteral("直流快充 · 120kW");
        record.durationMinutes = 60;
        record.estimatedFeeCents = 120;
        record.distanceMeters = 1500;
        return record;
    };

    struct Case
    {
        int secsToExpire;
        QString tone;
    };
    const QList<Case> cases = {
        {45 * 60, QStringLiteral("green")},
        {10 * 60, QStringLiteral("yellow")},
        {2 * 60, QStringLiteral("red")},
    };
    for (const auto& testCase : cases) {
        service->setMockRecords({makeActiveRecord(9100, testCase.secsToExpire)});
        module->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(
            order->viewState() == ReservationOrderPage::PageState::Active, 3000);
        QVERIFY(order->countdownText().contains(QLatin1Char(':')));
        QCOMPARE(order->countdownColorRole(), testCase.tone);
    }

    // 归零流转：3 秒后到期 → 自动流转“已过期”，订单页回空态、归档页出现。
    service->setMockRecords({makeActiveRecord(9101, 3)});
    module->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(
        order->viewState() == ReservationOrderPage::PageState::Active, 3000);
    QCOMPARE(order->countdownColorRole(), QStringLiteral("red"));
    QTRY_VERIFY_WITH_TIMEOUT(
        order->viewState() == ReservationOrderPage::PageState::Empty
            && module->completedPage()->recordCardCount() == 1
            && !service->hasUnfinishedReservation(),
        8000);
    saveSnapshotIfRequested(shell, QStringLiteral("home_shell_countdown.png"));
}

void HomeShellTest::moduleRouteBackReturnsToProfile()
{
    // 模块顶部“返回”（复用全局导航）→ 回“我的”Tab。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* topBar = shell.findChild<charging::client::TopNavBar*>();

    openModuleViaProfileTab(shell);
    QCOMPARE(pageStack->currentIndex(), 6);
    auto* backButton = shell.findChild<QPushButton*>(QStringLiteral("navBackButton"));
    QVERIFY(backButton->isVisible());
    backButton->click();
    QCOMPARE(pageStack->currentIndex(), 3);
    QVERIFY(tabButton(shell, QStringLiteral("profile"))->isChecked());
    QVERIFY(!topBar->isBackVisible());
}

void HomeShellTest::chargingOrderRoutesToChargingPage()
{
    // 修复回归：充电中订单点击应进入“充电过程”路由页（停止充电唯一入口），
    // 而不是只读的订单详情页（否则用户在壳层内无法结束会话）。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* topBar = shell.findChild<charging::client::TopNavBar*>();
    auto* orderList = shell.findChild<charging::client::OrderListPage*>();
    auto* chargingPage = shell.findChild<charging::client::ChargingPage*>();
    QVERIFY(orderList != nullptr);
    QVERIFY(chargingPage != nullptr);

    charging::client::OrderSummary live;
    live.order.id = 101;
    live.order.status = charging::model::OrderStatus::Charging;
    live.stationName = QStringLiteral("测试电站");
    live.chargerCode = QStringLiteral("DC-01");
    emit orderList->orderOpened(live);

    QCOMPARE(pageStack->currentWidget(), static_cast<QWidget*>(chargingPage));
    QVERIFY(topBar->isBackVisible());
    // 嵌入壳层：页内返回隐藏，返回动作由顶部全局导航承担。
    QPushButton* inPageBack = nullptr;
    for (auto* button : chargingPage->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("返回")) {
            inPageBack = button;
            break;
        }
    }
    QVERIFY(inPageBack != nullptr);
    QVERIFY(!inPageBack->isVisible());
}

void HomeShellTest::settlementDoneReturnsToFilteredOrderTab()
{
    // 修复回归：支付完成点“查看订单”应切到订单 Tab 并按“已完成”筛选——
    // 此前 setCurrentTab 同 id 不发信号，按钮表现为无响应。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* topBar = shell.findChild<charging::client::TopNavBar*>();
    auto* orderList = shell.findChild<charging::client::OrderListPage*>();
    auto* orderDetail = shell.findChild<charging::client::OrderDetailPage*>();
    auto* settlement = shell.findChild<charging::client::SettlementPage*>();
    QVERIFY(orderList != nullptr);
    QVERIFY(orderDetail != nullptr);
    QVERIFY(settlement != nullptr);

    charging::client::OrderSummary pending;
    pending.order.id = 102;
    pending.order.status = charging::model::OrderStatus::WaitingPayment;
    pending.order.amountCents = 2442;
    pending.stationName = QStringLiteral("测试电站");
    emit orderList->orderOpened(pending);
    QCOMPARE(pageStack->currentWidget(), static_cast<QWidget*>(orderDetail));

    emit orderDetail->payRequested();
    QCOMPARE(pageStack->currentWidget(), static_cast<QWidget*>(settlement));

    emit settlement->doneRequested();
    QCOMPARE(pageStack->currentIndex(), 1); // 订单 Tab
    QVERIFY(tabButton(shell, QStringLiteral("order"))->isChecked());
    QVERIFY(!topBar->isBackVisible());      // 返回栈已清
    // 筛选芯片同步为“已完成”（showFilter 同步生效，数据异步刷新）。
    QPushButton* completedChip = nullptr;
    for (auto* button : orderList->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("已完成")) {
            completedChip = button;
            break;
        }
    }
    QVERIFY(completedChip != nullptr);
    QVERIFY(completedChip->isChecked());
}

void HomeShellTest::rechargeFromSettlementReturnsAndUnlocksPay()
{
    // 修复回归：余额不足 → 去充值 → 充值成功后应自动回跳结算页，且结算页
    // 拿到权威新余额解锁“确认支付”。此前回跳后余额仍是进页旧快照，按钮置灰。
    HomeShell shell(makeSampleUser()); // 余额 ¥123.45
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* orderList = shell.findChild<charging::client::OrderListPage*>();
    auto* orderDetail = shell.findChild<charging::client::OrderDetailPage*>();
    auto* settlement = shell.findChild<charging::client::SettlementPage*>();
    auto* recharge = shell.findChild<charging::client::RechargePage*>();
    QVERIFY(recharge != nullptr);

    charging::client::OrderSummary pending;
    pending.order.id = 103;
    pending.order.status = charging::model::OrderStatus::WaitingPayment;
    pending.order.amountCents = 24420; // ¥244.20 > 余额 → 买不起
    pending.stationName = QStringLiteral("测试电站");
    emit orderList->orderOpened(pending);
    emit orderDetail->payRequested();
    QCOMPARE(pageStack->currentWidget(), static_cast<QWidget*>(settlement));

    // 结算页初始：支付置灰、“去充值”可见（余额不足 UI 门槛）。
    QPushButton* payButton = nullptr;
    QPushButton* jumpRecharge = nullptr;
    for (auto* button : settlement->findChildren<QPushButton*>()) {
        if (button->text().startsWith(QStringLiteral("确认支付"))) {
            payButton = button;
        } else if (button->text() == QStringLiteral("去充值")) {
            jumpRecharge = button;
        }
    }
    QVERIFY(payButton != nullptr);
    QVERIFY(!payButton->isEnabled());
    QVERIFY(jumpRecharge != nullptr);
    QVERIFY(jumpRecharge->isVisible());

    emit settlement->rechargeRequested();
    QCOMPARE(pageStack->currentWidget(), static_cast<QWidget*>(recharge));

    // 充值成功（直接发公共信号，绕开 mock 时延）：应自动回结算页并解锁支付。
    emit recharge->rechargeSucceeded(999900);
    QCOMPARE(pageStack->currentWidget(), static_cast<QWidget*>(settlement));
    QVERIFY(payButton->isEnabled());
    QVERIFY(!jumpRecharge->isVisible());
    for (auto* label : settlement->findChildren<QLabel*>()) {
        if (label->text().startsWith(QStringLiteral("当前余额"))) {
            QVERIFY(label->text().contains(QStringLiteral("9999")));
            QVERIFY(!label->text().contains(QStringLiteral("123.45")));
        }
    }
}

void HomeShellTest::emptyOrderListNoticeFillsListArea()
{
    // 修复回归：订单为空时空态提示应铺满列表区（此前滚动区隐藏后无拉伸项，
    // 提示缩在顶部一小条、下方大片空白）。
    HomeShell shell(makeSampleUser());
    shell.resize(420, 860);
    shell.show();
    QVERIFY(QTest::qWaitForWindowExposed(&shell));

    auto* orderList = shell.findChild<charging::client::OrderListPage*>();
    auto* orderService = shell.findChild<charging::client::OrderService*>();
    auto* listStack = orderList->findChild<QStackedWidget*>(QStringLiteral("uiOrderListStack"));
    auto* notice = orderList->findChild<charging::client::NoticePanel*>();
    QVERIFY(orderService != nullptr);
    QVERIFY(listStack != nullptr);
    QVERIFY(notice != nullptr);

    emit orderService->ordersLoaded({}, 0, false);
    QCOMPARE(listStack->currentWidget(), static_cast<QWidget*>(notice));
    QVERIFY(notice->height() >= listStack->height() - 10); // 铺满而非缩在顶部
}

QTEST_MAIN(HomeShellTest)

#include "tst_home_shell.moc"
