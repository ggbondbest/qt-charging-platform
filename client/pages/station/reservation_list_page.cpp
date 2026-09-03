#include "pages/station/reservation_list_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVariant>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

const char* kReservationPageStyleSheet = R"(
QLabel#reservationCancelErrorLabel {
    color: #E5484D;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#reservationCancelButton {
    background: #FFFFFF;
    color: #E5484D;
    border: 1px solid #E5484D;
    border-radius: 14px;
    padding: 5px 14px;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#reservationCancelButton:disabled {
    background: #F4F6F8;
    color: #9AA5B1;
    border: 1px solid #D5DCE4;
}
)";

struct StatusView
{
    const char* text;
    StatusTag::Tone tone;
};

// 预约状态 → 展示文案与色调（预约中/已完成/已取消/已过期）。
StatusView statusView(charging::model::ReservationStatus status)
{
    using charging::model::ReservationStatus;
    switch (status) {
    case ReservationStatus::Active:
        return {"预约中", StatusTag::Tone::Info};
    case ReservationStatus::Fulfilled:
        return {"已完成", StatusTag::Tone::Success};
    case ReservationStatus::Cancelled:
        return {"已取消", StatusTag::Tone::Neutral};
    case ReservationStatus::Expired:
        return {"已过期", StatusTag::Tone::Warning};
    }
    return {"未知", StatusTag::Tone::Neutral};
}

void clearLayoutItems(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

ReservationListPage::ReservationListPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("reservationListPage"));
    setStyleSheet(QString::fromLatin1(kReservationPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("我的预约"), this);
    titleLabel->setObjectName(QStringLiteral("reservationPageTitle"));
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    rootLayout->addWidget(titleLabel);

    // 取消失败等非阻断错误条（列表保持可见）。
    cancelErrorLabel_ = new QLabel(this);
    cancelErrorLabel_->setObjectName(QStringLiteral("reservationCancelErrorLabel"));
    cancelErrorLabel_->setWordWrap(true);
    cancelErrorLabel_->hide();
    rootLayout->addWidget(cancelErrorLabel_);

    pageStack_ = new QStackedWidget(this);
    pageStack_->setObjectName(QStringLiteral("reservationPageStack"));
    rootLayout->addWidget(pageStack_, 1);

    loadingPage_ = new QWidget(pageStack_);
    auto* loadingLayout = new QVBoxLayout(loadingPage_);
    auto* loadingLabel = new QLabel(tr("⏳ 正在加载预约记录…"), loadingPage_);
    loadingLabel->setObjectName(QStringLiteral("reservationLoadingLabel"));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setProperty("role", QStringLiteral("secondary"));
    loadingLayout->addStretch();
    loadingLayout->addWidget(loadingLabel);
    loadingLayout->addStretch();
    pageStack_->addWidget(loadingPage_);

    errorNotice_ = new NoticePanel(QStringLiteral("⚠️"), tr("预约记录加载失败"), QString(),
                                   tr("重试"), pageStack_);
    errorNotice_->setObjectName(QStringLiteral("reservationErrorNotice"));
    connect(errorNotice_, &NoticePanel::actionTriggered, this, &ReservationListPage::refresh);
    pageStack_->addWidget(errorNotice_);

    // 空数据：友好空页面提示（规格禁止大片空白）。
    emptyNotice_ = new NoticePanel(QStringLiteral("📒"), tr("暂无预约记录"),
                                   tr("在站点详情页选择空闲充电桩即可发起预约；预约成功后会在这里管理。"),
                                   QString(), pageStack_);
    emptyNotice_->setObjectName(QStringLiteral("reservationEmptyNotice"));
    pageStack_->addWidget(emptyNotice_);

    scrollArea_ = new QScrollArea(pageStack_);
    scrollArea_->setObjectName(QStringLiteral("reservationRecordsScroll"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    listPage_ = new QWidget(scrollArea_);
    listLayout_ = new QVBoxLayout(listPage_);
    listLayout_->setContentsMargins(0, 0, 8, 0);
    listLayout_->setSpacing(10);
    scrollArea_->setWidget(listPage_);
    pageStack_->addWidget(scrollArea_);

    setState(State::Loading);
}

void ReservationListPage::setService(services::reservation::ReservationService* service)
{
    if (service_ == service) {
        return;
    }
    service_ = service;
    if (service_ != nullptr) {
        connect(service_, &services::reservation::ReservationService::listStarted, this,
                &ReservationListPage::handleListStarted);
        connect(service_, &services::reservation::ReservationService::listSucceeded, this,
                &ReservationListPage::handleListSucceeded);
        connect(service_, &services::reservation::ReservationService::listFailed, this,
                &ReservationListPage::handleListFailed);
        connect(service_, &services::reservation::ReservationService::cancelSucceeded, this,
                &ReservationListPage::handleCancelSucceeded);
        connect(service_, &services::reservation::ReservationService::cancelFailed, this,
                &ReservationListPage::handleCancelFailed);
    }
}

services::reservation::ReservationService* ReservationListPage::service() const
{
    return service_;
}

void ReservationListPage::refresh()
{
    if (service_ == nullptr) {
        errorNotice_->setContent(QStringLiteral("⚠️"), tr("预约记录加载失败"),
                                 tr("预约服务尚未就绪，请稍后重试。"), tr("重试"));
        setState(State::Error);
        return;
    }
    service_->fetchList();
}

ReservationListPage::State ReservationListPage::viewState() const
{
    return viewState_;
}

int ReservationListPage::recordCardCount() const
{
    int count = 0;
    for (int i = 0; i < listLayout_->count(); ++i) {
        const auto* item = listLayout_->itemAt(i);
        if (item != nullptr && item->widget() != nullptr
            && item->widget()->property("isReservationCard").toBool()) {
            ++count;
        }
    }
    return count;
}

void ReservationListPage::setState(State state)
{
    viewState_ = state;
    switch (state) {
    case State::Loading:
        pageStack_->setCurrentWidget(loadingPage_);
        break;
    case State::Error:
        pageStack_->setCurrentWidget(errorNotice_);
        break;
    case State::Empty:
        pageStack_->setCurrentWidget(emptyNotice_);
        break;
    case State::List:
        pageStack_->setCurrentWidget(scrollArea_);
        break;
    }
}

void ReservationListPage::handleListStarted()
{
    setState(State::Loading);
}

void ReservationListPage::handleListSucceeded(
    const services::reservation::ReservationList& records)
{
    records_ = records;
    cancelErrorLabel_->hide();
    rebuildCards();
    setState(records_.isEmpty() ? State::Empty : State::List);
}

void ReservationListPage::handleListFailed(const QString& message)
{
    errorNotice_->setContent(QStringLiteral("⚠️"), tr("预约记录加载失败"), message, tr("重试"));
    setState(State::Error);
}

void ReservationListPage::handleCancelSucceeded(qint64 reservationId)
{
    Q_UNUSED(reservationId)
    cancelErrorLabel_->hide();
    refresh(); // 重新拉取列表，体现“已取消”状态与按钮置灰。
}

void ReservationListPage::handleCancelFailed(const QString& message)
{
    cancelErrorLabel_->setText(tr("⚠️ %1").arg(message));
    cancelErrorLabel_->show();
    emit cancelFailed(message);
}

void ReservationListPage::clearRows()
{
    clearLayoutItems(listLayout_);
}

void ReservationListPage::rebuildCards()
{
    clearRows();
    for (const auto& record : records_) {
        listLayout_->addWidget(createRecordCard(record));
    }
    listLayout_->addStretch();
}

QWidget* ReservationListPage::createRecordCard(
    const services::reservation::ReservationRecord& record)
{
    auto* card = new Card(listPage_);
    card->setProperty("isReservationCard", true);
    card->setProperty("reservationId", record.reservation.id);
    card->setProperty("reservationStatus", static_cast<int>(record.reservation.status));

    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    auto* nameLabel = new QLabel(record.stationName, card);
    nameLabel->setProperty("role", QStringLiteral("sectionTitle"));
    const auto view = statusView(record.reservation.status);
    auto* statusTag = new StatusTag(tr(view.text), view.tone, card);
    statusTag->setObjectName(QStringLiteral("reservationStatusTag"));
    titleRow->addWidget(nameLabel);
    titleRow->addStretch();
    titleRow->addWidget(statusTag);
    body->addLayout(titleRow);

    auto* infoLabel = new QLabel(
        tr("充电桩 %1 · 预约时长 %2 分钟").arg(record.chargerCode).arg(record.durationMinutes),
        card);
    infoLabel->setProperty("role", QStringLiteral("secondary"));
    body->addWidget(infoLabel);

    auto* timeRow = new QHBoxLayout();
    auto* timeLabel = new QLabel(
        tr("预约时间 %1")
            .arg(record.reservation.reservedAtUtc.toLocalTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm"))),
        card);
    timeLabel->setProperty("role", QStringLiteral("secondary"));
    auto* feeLabel = new QLabel(
        tr("¥%1").arg(QString::number(record.estimatedFeeCents / 100.0, 'f', 2)), card);
    feeLabel->setProperty("role", QStringLiteral("amountStrong"));
    timeRow->addWidget(timeLabel);
    timeRow->addStretch();
    timeRow->addWidget(feeLabel);
    body->addLayout(timeRow);

    // 取消预约：仅“预约中”可操作，其余状态置灰不可点（规格交互约束）。
    auto* cancelButton = new QPushButton(tr("取消预约"), card);
    cancelButton->setObjectName(QStringLiteral("reservationCancelButton"));
    cancelButton->setEnabled(record.reservation.status
                             == charging::model::ReservationStatus::Active);
    if (cancelButton->isEnabled()) {
        cancelButton->setCursor(Qt::PointingHandCursor);
    } else {
        cancelButton->setToolTip(tr("该预约已结束，无法取消"));
    }
    const qint64 reservationId = record.reservation.id;
    connect(cancelButton, &QPushButton::clicked, this, [this, reservationId]() {
        if (service_ != nullptr) {
            cancelErrorLabel_->hide();
            service_->cancel(reservationId);
        }
    });
    body->addWidget(cancelButton, 0, Qt::AlignRight);

    return card;
}

} // namespace charging::client::pages::station
