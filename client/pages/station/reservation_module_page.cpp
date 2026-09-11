// reservation_module_page.cpp —— 预约模块容器页的实现（成员 2，任务 #17
// 预约改版批）：“我的预约”拆为【预约订单】+【已完成的预约】两个子页，
// 由本容器用模块内二级 Tab 承载（全局顶/底 Tab 外壳不动，复用任务 #2 组件）。
//
// 职责：单点持有 ReservationService 的列表通道（fetchList → listStarted/
// listSucceeded/listFailed），按状态把一次结果分发（fan-out）给两个子页，
// 并同步加载/错误态；子页不自行订阅列表信号（订单页只订阅取消过程信号）。
// 数据流向：本模块 → ReservationService（双通道，真实走 TCP 契约，模拟走
// 本地）→ 信号回流 → 分发子页；取消成功/倒计时归零回流后重新 fetchList。
// 谁在用：HomeShell 底部“我的预约”入口与预约成功引导路由到本页。
#include "pages/station/reservation_module_page.h"

#include "pages/station/platform_theme.h"
#include "pages/station/reservation_completed_page.h"
#include "pages/station/reservation_order_page.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

// 二级 Tab 局部样式：仅在本模块内生效，不改全局 QSS。
const char* kModuleStyleSheet = R"(
QPushButton[isReservationSubTab="true"] {
    background: transparent;
    color: #6B7280;
    border: none;
    border-bottom: 3px solid transparent;
    padding: 8px 14px;
    font-size: 14px;
    font-weight: 600;
}
QPushButton[isReservationSubTab="true"][isSubTabActive="true"] {
    color: #00A76D;
    border-bottom: 3px solid #00B578;
}
)";

} // namespace

// 构造：标题 + 二级 Tab 按钮栏 + QStackedWidget 承载两子页（父对象即栈，
// 生命周期由 Qt 树接管）。子页间的跨页联动在此接线：订单页空态“去找桩”
// 原样上抛给宿主；归档页错误态“重试”直接绑回本模块 refresh——两条都走
// 信号而非互相持有，子页保持可独立测试。默认停在【预约订单】Tab。
ReservationModulePage::ReservationModulePage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("reservationModulePage"));
    setStyleSheet(QString::fromLatin1(kModuleStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(8);

    auto* titleLabel = new QLabel(tr("我的预约"), this);
    titleLabel->setObjectName(QStringLiteral("reservationModuleTitle"));
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    rootLayout->addWidget(titleLabel);

    // 二级 Tab 栏（仅模块内生效）：预约订单 / 已完成的预约。
    auto* tabBar = new QHBoxLayout();
    tabBar->setSpacing(6);
    orderTabButton_ = new QPushButton(tr("🕒 预约订单"), this);
    orderTabButton_->setObjectName(QStringLiteral("reservationOrderTabButton"));
    orderTabButton_->setProperty("isReservationSubTab", true);
    orderTabButton_->setCheckable(true);
    orderTabButton_->setCursor(Qt::PointingHandCursor);
    completedTabButton_ = new QPushButton(tr("📒 已完成的预约"), this);
    completedTabButton_->setObjectName(QStringLiteral("reservationHistoryTabButton"));
    completedTabButton_->setProperty("isReservationSubTab", true);
    completedTabButton_->setCheckable(true);
    completedTabButton_->setCursor(Qt::PointingHandCursor);
    tabBar->addWidget(orderTabButton_);
    tabBar->addWidget(completedTabButton_);
    tabBar->addStretch();
    rootLayout->addLayout(tabBar);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("reservationModuleStack"));
    orderPage_ = new ReservationOrderPage(stack_);
    completedPage_ = new ReservationCompletedPage(stack_);
    stack_->addWidget(orderPage_);
    stack_->addWidget(completedPage_);
    rootLayout->addWidget(stack_, 1);

    connect(orderTabButton_, &QPushButton::clicked, this,
            [this]() { switchTab(QStringLiteral("order")); });
    connect(completedTabButton_, &QPushButton::clicked, this,
            [this]() { switchTab(QStringLiteral("completed")); });
    connect(orderPage_, &ReservationOrderPage::findStationRequested, this,
            &ReservationModulePage::findStationRequested);
    connect(completedPage_, &ReservationCompletedPage::retryRequested, this,
            &ReservationModulePage::refresh);

    switchTab(QStringLiteral("order"));
}

// 注入服务（幂等闸同确认页）。分发原则：列表三信号由模块独占接收再转投
// 子页（避免两次 fetch 结果被两处各自解析、口径漂移）；订单页因要处理取消
// 过程信号才持有 service_，归档页纯展示、连服务指针都不给。cancelSucceeded
// 与 reservationExpired 都触发“切归档 Tab + 重拉”，与规格及倒计时流转对齐。
void ReservationModulePage::setService(
    services::reservation::ReservationService* service)
{
    if (service_ == service) {
        return;
    }
    service_ = service;
    orderPage_->setService(service_);
    if (service_ != nullptr) {
        connect(service_, &services::reservation::ReservationService::listStarted, this, [this]() {
            orderPage_->showLoading();
            completedPage_->showLoading();
        });
        connect(service_, &services::reservation::ReservationService::listSucceeded, this,
                &ReservationModulePage::handleListSucceeded);
        connect(service_, &services::reservation::ReservationService::listFailed, this,
                [this](const QString& message) {
                    orderPage_->showError(message);
                    completedPage_->showError(message);
                });
        // 取消成功：按规格跳转【已完成的预约】页面并刷新归档列表。
        connect(service_, &services::reservation::ReservationService::cancelSucceeded, this,
                [this](qint64) {
                    showCompletedTab();
                    refresh();
                });
        // 倒计时归零流转：重新拉取（订单页转空态、归档页出现过期记录）。
        connect(service_, &services::reservation::ReservationService::reservationExpired, this,
                [this](qint64) { refresh(); });
    }
}

// refresh：进入模块/重试/状态流转后统一走这里重拉。服务缺位时不静默——
// 两页同步呈现友好错误态（“桥缺位”也有确定展示，测试与演示共用此入口）。
void ReservationModulePage::refresh()
{
    if (service_ == nullptr) {
        const QString message = tr("预约服务尚未就绪，请稍后重试。");
        orderPage_->showError(message);
        completedPage_->showError(message);
        return;
    }
    service_->fetchList();
}

void ReservationModulePage::showOrderTab()
{
    switchTab(QStringLiteral("order"));
}

void ReservationModulePage::showCompletedTab()
{
    switchTab(QStringLiteral("completed"));
}

// —— 测试探针 / 宿主访问器（currentSubTab 供 Tab 态断言；两子页指针供端到端
// 测试驱动内部方法；service() 供演示切换模拟/真实开关，均为非拥有）——

QString ReservationModulePage::currentSubTab() const
{
    return stack_->currentWidget() == completedPage_ ? QStringLiteral("completed")
                                                     : QStringLiteral("order");
}

ReservationOrderPage* ReservationModulePage::orderPage() const
{
    return orderPage_;
}

ReservationCompletedPage* ReservationModulePage::completedPage() const
{
    return completedPage_;
}

services::reservation::ReservationService* ReservationModulePage::service() const
{
    return service_;
}

void ReservationModulePage::handleListSucceeded(
    const services::reservation::ReservationList& records)
{
    // 列表按状态分发：至多一条“预约中”（业务约束）→ 订单页；其余 → 归档页。
    // active==nullptr 才认领：万一数据里出现第二条“预约中”（脏数据/后端异常），
    // 它会被归入历史而不是覆盖第一条——展示口径保持确定、不崩不乱选。
    const services::reservation::ReservationRecord* active = nullptr;
    services::reservation::ReservationList history;
    for (const auto& record : records) {
        if (record.reservation.status == charging::model::ReservationStatus::Active
            && active == nullptr) {
            active = &record;
        } else {
            history.append(record);
        }
    }
    orderPage_->setActiveReservation(active);
    completedPage_->setHistory(history);
}

// switchTab：Tab 状态机单点——栈页、动态属性（驱动下划线高亮 QSS）、
// checkable 选中态三者必须原子地一起翻转，故收敛为一个私有入口，
// 用户点击/宿主路由（showOrderTab/showCompletedTab）/构造默认都走这里。
void ReservationModulePage::switchTab(const QString& id)
{
    const bool toCompleted = id == QLatin1String("completed");
    stack_->setCurrentWidget(toCompleted
                                 ? static_cast<QWidget*>(completedPage_)
                                 : static_cast<QWidget*>(orderPage_));
    orderTabButton_->setProperty("isSubTabActive", !toCompleted);
    completedTabButton_->setProperty("isSubTabActive", toCompleted);
    orderTabButton_->setChecked(!toCompleted);
    completedTabButton_->setChecked(toCompleted);
    // 动态属性变化不会自动重算 QSS，需手动 unpolish/polish 强制按新属性
    // 重选样式选择器（与订单页倒计时换色同一手法）。
    orderTabButton_->style()->unpolish(orderTabButton_);
    orderTabButton_->style()->polish(orderTabButton_);
    completedTabButton_->style()->unpolish(completedTabButton_);
    completedTabButton_->style()->polish(completedTabButton_);
}

} // namespace charging::client::pages::station
