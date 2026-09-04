#include "pages/station/reservation_order_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/notice_panel.h"
#include "pages/station/platform_theme.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

// 页面局部样式：倒计时三档色 + 取消按钮，仅本页生效，不改全局 QSS。
const char* kOrderPageStyleSheet = R"(
QLabel#reservationCountdownLabel[countdownTone="green"] {
    color: #00A76D;
    font-size: 30px;
    font-weight: 800;
}
QLabel#reservationCountdownLabel[countdownTone="yellow"] {
    color: #D48806;
    font-size: 30px;
    font-weight: 800;
}
QLabel#reservationCountdownLabel[countdownTone="red"] {
    color: #E5484D;
    font-size: 30px;
    font-weight: 800;
}
QPushButton#reservationOrderCancelButton {
    background: #FFFFFF;
    color: #E5484D;
    border: 1px solid #E5484D;
    border-radius: 16px;
    padding: 7px 18px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#reservationOrderCancelButton:disabled {
    color: #B9C4CF;
    border: 1px solid #D5DCE4;
}
QLabel#orderModuleCaption {
    color: #9AA5B1;
    font-size: 11px;
}
)";

constexpr int kGreenThresholdSecs = 30 * 60; // >30 分钟：绿色
constexpr int kYellowFloorSecs = 5 * 60;     // 5~30 分钟：黄色；<5 分钟：红色

QString formatClock(qint64 secs)
{
    const qint64 h = secs / 3600;
    const qint64 m = (secs % 3600) / 60;
    const qint64 s = secs % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace

ReservationOrderPage::ReservationOrderPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("reservationOrderPage"));
    setStyleSheet(QString::fromLatin1(kOrderPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* stack = new QStackedWidget(this);
    stack->setObjectName(QStringLiteral("reservationOrderStack"));
    stack_ = stack;
    rootLayout->addWidget(stack);

    // ① 加载中。
    loadingPage_ = new QWidget(stack);
    auto* loadingLayout = new QVBoxLayout(loadingPage_);
    auto* loadingLabel = new QLabel(tr("⏳ 正在加载预约信息…"), loadingPage_);
    loadingLabel->setObjectName(QStringLiteral("orderLoadingLabel"));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setProperty("role", QStringLiteral("secondary"));
    loadingLayout->addStretch();
    loadingLayout->addWidget(loadingLabel);
    loadingLayout->addStretch();
    stack->addWidget(loadingPage_);

    // ② 错误态（友好提示 + 重试经模块）。
    errorNotice_ = new NoticePanel(QStringLiteral("⚠️"), tr("预约订单加载失败"), QString(),
                                   tr("重试"), stack);
    errorNotice_->setObjectName(QStringLiteral("orderErrorNotice"));
    stack->addWidget(errorNotice_);

    // ③ 空态：无进行中预约，引导去找桩。
    emptyNotice_ = new NoticePanel(QStringLiteral("🅿️"), tr("暂无进行中的预约"),
                                   tr("去站点详情页挑选空闲充电桩，发起新的预约。"),
                                   tr("🔍 去找桩"), stack);
    emptyNotice_->setObjectName(QStringLiteral("orderEmptyNotice"));
    connect(static_cast<NoticePanel*>(emptyNotice_), &NoticePanel::actionTriggered, this,
            &ReservationOrderPage::findStationRequested);
    stack->addWidget(emptyNotice_);

    // ④ 有进行中预约：左-中-右三栏（滚动容器，支持鼠标滚轮）。
    auto* activeScroll = new QScrollArea(stack);
    activeScroll->setObjectName(QStringLiteral("orderActiveScroll"));
    activeScroll->setWidgetResizable(true);
    activeScroll->setFrameShape(QFrame::NoFrame);
    activeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    stack->addWidget(activeScroll);

    auto* columns = new QWidget(activeScroll);
    auto* columnsLayout = new QHBoxLayout(columns);
    columnsLayout->setContentsMargins(2, 2, 8, 2);
    columnsLayout->setSpacing(10);
    activeScroll->setWidget(columns);

    // 左栏：距离（虚拟数据，预留导航对接）。
    auto* leftCard = new Card(columns);
    leftCard->setProperty("isOrderSideCard", true);
    auto* leftBody = leftCard->bodyLayout();
    auto* leftTitle = new QLabel(tr("📍 距离"), leftCard);
    leftTitle->setProperty("role", QStringLiteral("sectionTitle"));
    distanceLabel_ = new QLabel(leftCard);
    distanceLabel_->setObjectName(QStringLiteral("orderDistanceLabel"));
    distanceLabel_->setProperty("role", QStringLiteral("amountStrong"));
    auto* leftHint = new QLabel(tr("虚拟数据 · 导航功能后续对接"), leftCard);
    leftHint->setObjectName(QStringLiteral("orderModuleCaption"));
    leftHint->setWordWrap(true);
    leftBody->addWidget(leftTitle);
    leftBody->addStretch();
    leftBody->addWidget(distanceLabel_);
    leftBody->addWidget(leftHint);
    leftBody->addStretch();
    columnsLayout->addWidget(leftCard, 1);

    // 中栏：预约信息 + 倒计时（每秒刷新）+ 取消预约。
    auto* centerCard = new Card(columns);
    centerCard->setProperty("isOrderCenterCard", true);
    auto* centerBody = centerCard->bodyLayout();
    auto* centerTitle = new QLabel(tr("⏱ 预约倒计时"), centerCard);
    centerTitle->setProperty("role", QStringLiteral("sectionTitle"));
    centerTitle->setAlignment(Qt::AlignCenter);
    activeInfoLabel_ = new QLabel(centerCard);
    activeInfoLabel_->setObjectName(QStringLiteral("orderActiveInfoLabel"));
    activeInfoLabel_->setAlignment(Qt::AlignCenter);
    activeInfoLabel_->setWordWrap(true);
    activeInfoLabel_->setProperty("role", QStringLiteral("secondary"));
    countdownLabel_ = new QLabel(centerCard);
    countdownLabel_->setObjectName(QStringLiteral("reservationCountdownLabel"));
    countdownLabel_->setAlignment(Qt::AlignCenter);
    expiresAtLabel_ = new QLabel(centerCard);
    expiresAtLabel_->setObjectName(QStringLiteral("orderExpiresAtLabel"));
    expiresAtLabel_->setAlignment(Qt::AlignCenter);
    expiresAtLabel_->setWordWrap(true);
    expiresAtLabel_->setProperty("role", QStringLiteral("caption"));
    cancelButton_ = new QPushButton(tr("取消预约"), centerCard);
    cancelButton_->setObjectName(QStringLiteral("reservationOrderCancelButton"));
    cancelButton_->setCursor(Qt::PointingHandCursor);
    connect(cancelButton_, &QPushButton::clicked, this,
            &ReservationOrderPage::handleCancelClicked);
    centerBody->addWidget(centerTitle);
    centerBody->addWidget(activeInfoLabel_);
    centerBody->addStretch();
    centerBody->addWidget(countdownLabel_);
    centerBody->addWidget(expiresAtLabel_);
    centerBody->addStretch();
    centerBody->addWidget(cancelButton_, 0, Qt::AlignCenter);
    columnsLayout->addWidget(centerCard, 2);

    // 右栏：汽车电量占位（业务暂不实现）。
    auto* rightCard = new Card(columns);
    rightCard->setProperty("isOrderSideCard", true);
    auto* rightBody = rightCard->bodyLayout();
    auto* rightTitle = new QLabel(tr("🔋 汽车电量"), rightCard);
    rightTitle->setProperty("role", QStringLiteral("sectionTitle"));
    batteryLabel_ = new QLabel(tr("SOC --%"), rightCard);
    batteryLabel_->setObjectName(QStringLiteral("orderBatteryLabel"));
    batteryLabel_->setProperty("role", QStringLiteral("amountStrong"));
    auto* rightHint = new QLabel(tr("虚拟占位 · 电量对接功能暂不实现"), rightCard);
    rightHint->setObjectName(QStringLiteral("orderModuleCaption"));
    rightHint->setWordWrap(true);
    rightBody->addWidget(rightTitle);
    rightBody->addStretch();
    rightBody->addWidget(batteryLabel_);
    rightBody->addWidget(rightHint);
    rightBody->addStretch();
    columnsLayout->addWidget(rightCard, 1);

    // 每秒刷新倒计时（规格：自动刷新一次/秒）。
    countdownTimer_ = new QTimer(this);
    countdownTimer_->setInterval(1000);
    connect(countdownTimer_, &QTimer::timeout, this, &ReservationOrderPage::applyCountdown);

    viewState_ = PageState::Loading;
    stack->setCurrentWidget(loadingPage_);
}

void ReservationOrderPage::setService(
    services::reservation::ReservationService* service)
{
    if (service_ == service) {
        return;
    }
    service_ = service;
    if (service_ != nullptr) {
        connect(service_, &services::reservation::ReservationService::cancelStarted, this,
                &ReservationOrderPage::handleCancelStarted);
        connect(service_, &services::reservation::ReservationService::cancelFailed, this,
                &ReservationOrderPage::handleCancelFailed);
    }
}

void ReservationOrderPage::setActiveReservation(
    const services::reservation::ReservationRecord* record)
{
    if (record == nullptr) {
        hasActive_ = false;
        countdownTimer_->stop();
        viewState_ = PageState::Empty;
        stack_->setCurrentWidget(emptyNotice_);
        return;
    }

    active_ = *record;
    hasActive_ = true;
    cancelling_ = false;
    cancelButton_->setEnabled(true);
    cancelButton_->setText(tr("取消预约"));
    const QString startText = active_.startAtUtc.isValid()
        ? active_.startAtUtc.toLocalTime().toString(QStringLiteral("HH:mm"))
        : QStringLiteral("--");
    const QString endText
        = active_.reservation.expiresAtUtc.toLocalTime().toString(QStringLiteral("HH:mm"));
    activeInfoLabel_->setText(
        tr("%1 · %2\n%3 · %4 分钟 · 预估 ¥%5\n车辆 %6 · 时段 %7—%8")
            .arg(active_.stationName, active_.chargerCode, active_.chargerSpec,
                 QString::number(active_.durationMinutes),
                 QString::number(active_.estimatedFeeCents / 100.0, 'f', 2),
                 active_.vehiclePlate.isEmpty() ? tr("未关联") : active_.vehiclePlate,
                 startText, endText));
    if (active_.distanceMeters >= 1000) {
        distanceLabel_->setText(
            tr("约 %1 km").arg(active_.distanceMeters / 1000.0, 0, 'f', 1));
    } else if (active_.distanceMeters >= 0) {
        distanceLabel_->setText(tr("约 %1 m").arg(active_.distanceMeters));
    } else {
        distanceLabel_->setText(tr("--"));
    }
    batteryLabel_->setText(tr("SOC --%（虚拟占位）"));
    applyCountdown();
    countdownTimer_->start();
    viewState_ = PageState::Active;
    stack_->setCurrentIndex(3);
}

void ReservationOrderPage::showLoading()
{
    hasActive_ = false;
    countdownTimer_->stop();
    viewState_ = PageState::Loading;
    stack_->setCurrentWidget(loadingPage_);
}

void ReservationOrderPage::showError(const QString& message)
{
    hasActive_ = false;
    countdownTimer_->stop();
    viewState_ = PageState::Error;
    static_cast<NoticePanel*>(errorNotice_)->setContent(QStringLiteral("⚠️"),
                                                        tr("预约订单加载失败"), message,
                                                        tr("重试"));
    stack_->setCurrentWidget(errorNotice_);
}

ReservationOrderPage::PageState ReservationOrderPage::viewState() const
{
    return viewState_;
}

QString ReservationOrderPage::countdownText() const
{
    return countdownLabel_->text();
}

QString ReservationOrderPage::countdownColorRole() const
{
    return countdownLabel_->property("countdownTone").toString();
}

QString ReservationOrderPage::distanceText() const
{
    return distanceLabel_->text();
}

QString ReservationOrderPage::batteryText() const
{
    return batteryLabel_->text();
}

void ReservationOrderPage::applyCountdown()
{
    if (!hasActive_) {
        countdownTimer_->stop();
        return;
    }
    // 迟到扫描（任务 #17 二次迭代）：每秒 tick 顺带驱动全库“开始 + 15 分钟
    // 未到站”自动取消（流转经 reservationExpired 信号回流模块刷新）。
    if (service_ != nullptr) {
        service_->cancelLateReservations();
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qint64 remaining = now.secsTo(active_.reservation.expiresAtUtc);
    if (remaining <= 0) {
        // 倒计时归零：状态自动流转（预约中 → 已过期），模块收到流转信号后
        // 重新拉取列表刷新展示。
        countdownTimer_->stop();
        countdownLabel_->setText(QStringLiteral("00:00"));
        countdownLabel_->setProperty("countdownTone", QStringLiteral("red"));
        countdownLabel_->style()->unpolish(countdownLabel_);
        countdownLabel_->style()->polish(countdownLabel_);
        if (service_ != nullptr) {
            service_->expireReservation(active_.reservation.id);
        }
        return;
    }

    auto setTone = [this](const QString& tone) {
        if (countdownLabel_->property("countdownTone").toString() != tone) {
            countdownLabel_->setProperty("countdownTone", tone);
            countdownLabel_->style()->unpolish(countdownLabel_);
            countdownLabel_->style()->polish(countdownLabel_);
        }
    };

    const bool waiting = active_.startAtUtc.isValid() && now < active_.startAtUtc;
    if (waiting) {
        // 阶段一：时段尚未开始——“距开始 mm:ss”（绿色等待态）。
        countdownLabel_->setText(
            tr("距开始 %1").arg(formatClock(now.secsTo(active_.startAtUtc))));
        setTone(QStringLiteral("green"));
        expiresAtLabel_->setText(tr("预约时段 %1—%2 · 等待开始")
                                     .arg(active_.startAtUtc.toLocalTime().toString(
                                              QStringLiteral("HH:mm")),
                                          active_.reservation.expiresAtUtc.toLocalTime().toString(
                                              QStringLiteral("HH:mm"))));
        return;
    }

    // 阶段二：时段进行中——剩余时长三档色（>30 分绿 / 5~30 分黄 / <5 分红）。
    countdownLabel_->setText(formatClock(remaining));
    setTone(remaining > kGreenThresholdSecs
        ? QStringLiteral("green")
        : (remaining >= kYellowFloorSecs ? QStringLiteral("yellow") : QStringLiteral("red")));
    expiresAtLabel_->setText(
        tr("有效至 %1（%2）")
            .arg(active_.reservation.expiresAtUtc.toString(QStringLiteral("HH:mm:ss")),
                 tr("%1 分 %2 秒后").arg(remaining / 60).arg(remaining % 60)));
}

void ReservationOrderPage::handleCancelClicked()
{
    if (cancelling_ || !hasActive_ || service_ == nullptr) {
        return;
    }
    service_->cancel(active_.reservation.id);
}

void ReservationOrderPage::handleCancelStarted(qint64 reservationId)
{
    if (!hasActive_ || reservationId != active_.reservation.id) {
        return;
    }
    // loading 态：取消中禁用按钮，防止重复提交。
    cancelling_ = true;
    countdownTimer_->stop();
    cancelButton_->setEnabled(false);
    cancelButton_->setText(tr("取消中…"));
}

void ReservationOrderPage::handleCancelFailed(const QString& message)
{
    if (!cancelling_) {
        return;
    }
    // 取消失败：页内提示不打断倒计时，可重试。
    cancelling_ = false;
    cancelButton_->setEnabled(true);
    cancelButton_->setText(tr("取消预约"));
    applyCountdown();
    countdownTimer_->start();
    expiresAtLabel_->setText(tr("⚠️ %1").arg(message));
}

} // namespace charging::client::pages::station
