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
    orderTabButton_->style()->unpolish(orderTabButton_);
    orderTabButton_->style()->polish(orderTabButton_);
    completedTabButton_->style()->unpolish(completedTabButton_);
    completedTabButton_->style()->polish(completedTabButton_);
}

} // namespace charging::client::pages::station
