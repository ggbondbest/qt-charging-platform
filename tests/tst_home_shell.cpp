#include "charging/client/widgets/top_nav_bar.h"
#include "pages/station/home_shell.h"
#include "pages/station/reservation_dialog.h"
#include "pages/station/reservation_list_page.h"
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
#include <QSignalSpy>
#include <QStackedWidget>
#include <QtTest>

namespace {

using HomeShell = charging::client::pages::station::HomeShell;
using ReservationDialog = charging::client::pages::station::ReservationDialog;
using ReservationListPage = charging::client::pages::station::ReservationListPage;
using StationDetailPage = charging::client::pages::station::StationDetailPage;
using StationHomePage = charging::client::pages::station::StationHomePage;

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

ReservationListPage* recordsPage(HomeShell& shell)
{
    auto* page = shell.reservationPage();
    Q_ASSERT_X(page != nullptr, "recordsPage", "HomeShell must own a ReservationListPage");
    return page;
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

// 打开站点详情并等待桩列表就绪（页面切到详情路由，保证弹窗父链可见）。
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

// 从“我的”Tab 入口进入预约记录路由页。
void openRecordsViaProfileTab(HomeShell& shell)
{
    tabButton(shell, QStringLiteral("profile"))->click();
    auto* entry = shell.findChild<QPushButton*>(QStringLiteral("openReservationsButton"));
    QVERIFY(entry != nullptr);
    entry->click();
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
    void profilePageShowsUserAndLogout();
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
    // —— 任务 #17：预约弹窗与预约记录 ——
    void reservationDialogSubmitRefreshesChargerState();
    void reservationDialogConflictKeepsDialogOpen();
    void reservationWithoutLoginPromptsAndRoutesToLogin();
    void recordsPageShowsFourStatusesAndCancelRules();
    void recordsPageCancelUpdatesState();
    void recordsPageEmptyAndErrorStates();
    void recordsRouteBackReturnsToProfile();
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
    // 4 个 Tab 页 + 详情路由页 + 预约记录路由页（均非 Tab）。
    QCOMPARE(pageStack->count(), 6);

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

void HomeShellTest::profilePageShowsUserAndLogout()
{
    HomeShell shell(makeSampleUser());

    QSignalSpy logoutSpy(&shell, &HomeShell::logoutRequested);
    tabButton(shell, QStringLiteral("profile"))->click();

    auto* nicknameLabel = shell.findChild<QLabel*>(QStringLiteral("nicknameLabel"));
    auto* balanceLabel = shell.findChild<QLabel*>(QStringLiteral("balanceLabel"));
    auto* logoutButton = shell.findChild<QPushButton*>(QStringLiteral("logoutButton"));
    QVERIFY(nicknameLabel != nullptr);
    QVERIFY(balanceLabel != nullptr);
    QVERIFY(nicknameLabel->text().contains(QStringLiteral("用户5678")));
    QVERIFY(balanceLabel->text().contains(QStringLiteral("123.45")));

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
    // 正常态：充电桩卡片列表 + 故障视觉标记 + 预约占位入口。
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

    // 空闲桩点击 → 弹出预约弹窗（挂在详情页上）。
    QVERIFY(enabledButton != nullptr);
    enabledButton->click();
    QCOMPARE(reservationSpy.count(), 1);
    auto* dialog = shell.findChild<ReservationDialog*>();
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    QCOMPARE(dialog->selectedMinutes(), 60); // 默认时长
    // 预估费用 = 电价 × 时长：id1 电价 120 分/度 × 60 分钟 = ¥1.20。
    QVERIFY(dialog->estimatedFeeText().contains(QStringLiteral("¥1.20")));
    auto* closeButton = shell.findChild<QPushButton*>(QStringLiteral("reservationCloseButton"));
    QVERIFY(closeButton != nullptr);
    closeButton->click(); // 弹窗支持手动关闭
    QTRY_VERIFY_WITH_TIMEOUT(shell.findChild<ReservationDialog*>() == nullptr, 3000);

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

void HomeShellTest::reservationDialogSubmitRefreshesChargerState()
{
    // 提交成功：loading 提交态 → 成功提示 → 弹窗自动关闭 → 桩状态刷新。
    HomeShell shell(makeSampleUser());
    shell.show();
    openDetailAndWait(shell, 1, 850);
    auto* detail = detailPage(shell);

    QPushButton* enabledButton = nullptr;
    for (auto* button : reserveButtons(*detail)) {
        if (button->isEnabled()) {
            enabledButton = button;
            break;
        }
    }
    QVERIFY(enabledButton != nullptr);
    enabledButton->click();
    QPointer<ReservationDialog> dialog(shell.findChild<ReservationDialog*>());
    QVERIFY(dialog != nullptr);

    auto* submit = dialog->findChild<QPushButton*>(QStringLiteral("reservationSubmitButton"));
    QVERIFY(submit != nullptr);
    submit->click();
    // 提交中：按钮禁用防重复提交（loading 态）。
    QVERIFY(!submit->isEnabled());
    QVERIFY(submit->text().contains(QStringLiteral("提交中")));
    saveSnapshotIfRequested(*dialog, QStringLiteral("home_shell_reservation_submitting.png"));

    // 成功：预约后桩变“已预约”，空闲数 3 → 2；弹窗展示提示后自动关闭。
    auto* summary = shell.findChild<QLabel*>(QStringLiteral("detailChargerSummaryLabel"));
    QVERIFY(summary != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains(QStringLiteral("空闲 2")), 5000);
    QCOMPARE(enabledCount(reserveButtons(*detail)), 2);
    QTRY_VERIFY_WITH_TIMEOUT(dialog == nullptr, 5000);
}

void HomeShellTest::reservationDialogConflictKeepsDialogOpen()
{
    // 并发边界：提交瞬间桩被抢占 → 展示原因、弹窗保持、可重试可关闭。
    HomeShell shell(makeSampleUser());
    shell.show();
    openDetailAndWait(shell, 1, 850);
    auto* detail = detailPage(shell);
    auto* service = recordsPage(shell)->service();
    QVERIFY(service != nullptr);
    service->setSimulateNextSubmitConflict(true);

    QPushButton* enabledButton = nullptr;
    for (auto* button : reserveButtons(*detail)) {
        if (button->isEnabled()) {
            enabledButton = button;
            break;
        }
    }
    QVERIFY(enabledButton != nullptr);
    enabledButton->click();
    auto* dialog = shell.findChild<ReservationDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QPushButton*>(QStringLiteral("reservationSubmitButton"))->click();

    QTRY_VERIFY_WITH_TIMEOUT(dialog->messageText().contains(QStringLiteral("抢占")), 5000);
    QVERIFY(dialog->isVisible()); // 失败不自动关闭
    auto* submit = dialog->findChild<QPushButton*>(QStringLiteral("reservationSubmitButton"));
    QVERIFY(submit->isEnabled()); // 按钮恢复可再次尝试
    QVERIFY(submit->text().contains(QStringLiteral("确认预约")));
    // 桩状态未被误改：失败不刷新为已预约。
    QCOMPARE(enabledCount(reserveButtons(*detail)), 3);
    saveSnapshotIfRequested(*dialog, QStringLiteral("home_shell_reservation_conflict.png"));

    dialog->findChild<QPushButton*>(QStringLiteral("reservationCloseButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(shell.findChild<ReservationDialog*>() == nullptr, 3000);
}

void HomeShellTest::reservationWithoutLoginPromptsAndRoutesToLogin()
{
    // 未登录点击预约：提示登录，“去登录”经全局 loginRequested 跳登录页。
    HomeShell shell;
    shell.show();
    openDetailAndWait(shell, 1, 850);
    auto* detail = detailPage(shell);

    QSignalSpy loginSpy(&shell, &HomeShell::loginRequested);
    QPushButton* enabledButton = nullptr;
    for (auto* button : reserveButtons(*detail)) {
        if (button->isEnabled()) {
            enabledButton = button;
            break;
        }
    }
    QVERIFY(enabledButton != nullptr);
    enabledButton->click();

    auto* prompt = shell.findChild<QMessageBox*>(QStringLiteral("reservationLoginPrompt"));
    QVERIFY(prompt != nullptr);
    QVERIFY(prompt->isVisible());
    // 未登录不弹预约表单。
    QVERIFY(shell.findChild<ReservationDialog*>() == nullptr);

    auto* goLogin = prompt->findChild<QPushButton*>(QStringLiteral("reservationGoLoginButton"));
    QVERIFY(goLogin != nullptr);
    goLogin->click();
    QCOMPARE(loginSpy.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        shell.findChild<QMessageBox*>(QStringLiteral("reservationLoginPrompt")) == nullptr, 3000);
}

void HomeShellTest::recordsPageShowsFourStatusesAndCancelRules()
{
    // 记录页：四状态卡片；仅“预约中”可取消，其余置灰。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));

    openRecordsViaProfileTab(shell);
    QCOMPARE(pageStack->currentIndex(), 5);
    auto* topBar = shell.findChild<charging::client::TopNavBar*>();
    QVERIFY(topBar->isBackVisible());

    auto* records = recordsPage(shell);
    QVERIFY(records->viewState() == ReservationListPage::State::Loading
            || records->viewState() == ReservationListPage::State::List);
    QTRY_VERIFY_WITH_TIMEOUT(records->viewState() == ReservationListPage::State::List, 3000);
    QCOMPARE(records->recordCardCount(), 4);

    QList<QPushButton*> cancelButtons;
    const auto all = records->findChildren<QPushButton*>();
    for (auto* button : all) {
        if (button->objectName() == QStringLiteral("reservationCancelButton")) {
            cancelButtons.append(button);
        }
    }
    QCOMPARE(cancelButtons.size(), 4);
    QCOMPARE(enabledCount(cancelButtons), 1); // 仅“预约中”可取消
    for (const auto* button : cancelButtons) {
        if (!button->isEnabled()) {
            QVERIFY(!button->toolTip().isEmpty());
        }
    }

    QString allText;
    for (const auto* label : records->findChildren<QLabel*>()) {
        allText += label->text();
    }
    QVERIFY(allText.contains(QStringLiteral("预约中")));
    QVERIFY(allText.contains(QStringLiteral("已完成")));
    QVERIFY(allText.contains(QStringLiteral("已取消")));
    QVERIFY(allText.contains(QStringLiteral("已过期")));
    QVERIFY(allText.contains(QStringLiteral("南山智造充电站"))); // 站点名称
    QVERIFY(allText.contains(QStringLiteral("SZ-NSZ-03-07")));   // 桩编号
    saveSnapshotIfRequested(*records, QStringLiteral("home_shell_reservations.png"));
}

void HomeShellTest::recordsPageCancelUpdatesState()
{
    // 取消“预约中”记录：成功后刷新列表，该记录转为“已取消”且按钮置灰。
    HomeShell shell(makeSampleUser());
    shell.show();
    openRecordsViaProfileTab(shell);

    auto* records = recordsPage(shell);
    QTRY_VERIFY_WITH_TIMEOUT(records->viewState() == ReservationListPage::State::List, 3000);

    QList<QPushButton*> cancelButtons;
    const auto all = records->findChildren<QPushButton*>();
    for (auto* button : all) {
        if (button->objectName() == QStringLiteral("reservationCancelButton")) {
            cancelButtons.append(button);
        }
    }
    QPushButton* activeCancel = nullptr;
    for (auto* button : cancelButtons) {
        if (button->isEnabled()) {
            activeCancel = button;
            break;
        }
    }
    QVERIFY(activeCancel != nullptr);
    activeCancel->click();

    // 取消 → 自动刷新：卡片仍 4 张，可取消按钮归零。
    QTRY_VERIFY_WITH_TIMEOUT(
        records->viewState() == ReservationListPage::State::List
            && records->recordCardCount() == 4
            && [&records]() {
                   QList<QPushButton*> buttons;
                   for (auto* button : records->findChildren<QPushButton*>()) {
                       if (button->objectName() == QStringLiteral("reservationCancelButton")) {
                           buttons.append(button);
                       }
                   }
                   return enabledCount(buttons) == 0;
               }(),
        5000);
    QCOMPARE(shell.findChild<QLabel*>(QStringLiteral("reservationCancelErrorLabel"))
                 ->isHidden(),
             true); // 无错误条
}

void HomeShellTest::recordsPageEmptyAndErrorStates()
{
    // 空记录 → 友好空页面；接口报错 → 错误态 + 重试恢复。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* service = recordsPage(shell)->service();
    QVERIFY(service != nullptr);
    service->setMockRecords({});

    openRecordsViaProfileTab(shell);
    auto* records = recordsPage(shell);
    QTRY_VERIFY_WITH_TIMEOUT(records->viewState() == ReservationListPage::State::Empty, 3000);
    QCOMPARE(records->recordCardCount(), 0);
    auto* emptyNotice = shell.findChild<QWidget*>(QStringLiteral("reservationEmptyNotice"));
    QVERIFY(emptyNotice != nullptr);
    QVERIFY(emptyNotice->isVisible());
    saveSnapshotIfRequested(*records, QStringLiteral("home_shell_reservations_empty.png"));

    service->setSimulateFailure(true);
    records->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(records->viewState() == ReservationListPage::State::Error, 3000);
    QPushButton* retryButton = nullptr;
    for (auto* button : records->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("重试")) {
            retryButton = button;
            break;
        }
    }
    QVERIFY(retryButton != nullptr);
    retryButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(records->viewState() == ReservationListPage::State::Empty, 3000);
}

void HomeShellTest::recordsRouteBackReturnsToProfile()
{
    // 记录页顶部“返回”（复用全局导航）→ 回“我的”Tab。
    HomeShell shell(makeSampleUser());
    shell.show();
    auto* pageStack = shell.findChild<QStackedWidget*>(QStringLiteral("homePageStack"));
    auto* topBar = shell.findChild<charging::client::TopNavBar*>();

    openRecordsViaProfileTab(shell);
    QCOMPARE(pageStack->currentIndex(), 5);
    auto* backButton = shell.findChild<QPushButton*>(QStringLiteral("navBackButton"));
    QVERIFY(backButton->isVisible());
    backButton->click();
    QCOMPARE(pageStack->currentIndex(), 3);
    QVERIFY(tabButton(shell, QStringLiteral("profile"))->isChecked());
    QVERIFY(!topBar->isBackVisible());
}

QTEST_MAIN(HomeShellTest)

#include "tst_home_shell.moc"
