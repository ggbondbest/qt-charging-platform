// reservation_completed_page.cpp —— 已完成的预约页（归档列表）的实现
// （成员 2，任务 #17 预约改版批：预约模块二级 Tab 之二）。
//
// 职责：把模块分发来的全部“已结束”记录（已完成/已取消/已过期，含迟到
// 自动取消的特殊文案）渲染为可点击历史卡片，点击弹出只读详情对话框；
// 外加 加载中/空态/错误(可重试) 三种边界视图（QStackedWidget 四页切换）。
//
// 数据流向：本页面不直连任何服务——纯“视图从模型”：ReservationModulePage
// 一次 fetchList 后按状态过滤，经 setHistory 注入整份列表；错误态“重试”
// 只发 retryRequested 信号由模块重新拉取。
#include "pages/station/reservation_completed_page.h"

#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

const char* kCompletedPageStyleSheet = R"(
QPushButton#reservationDetailCloseButton {
    background: #F4F6F8;
    color: #1F2937;
    border: 1px solid #D5DCE4;
    border-radius: 16px;
    padding: 7px 18px;
    font-size: 13px;
    font-weight: 600;
}
)";

struct StatusView
{
    const char* text;
    StatusTag::Tone tone;
};

// 预约状态 → 展示文案与色调（与预约模块统一语义）。
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

// 迟到取消特殊文案（任务 #17 二次迭代）：区分“主动取消”与“超时未到自动取消”。
QString statusTextFor(const services::reservation::ReservationRecord& record)
{
    using charging::model::ReservationStatus;
    if (record.lateCancelled && record.reservation.status == ReservationStatus::Cancelled) {
        return QObject::tr("已取消·迟到");
    }
    return QObject::tr(statusView(record.reservation.status).text);
}

// 预约时段文案（开始—结束）；无开始时刻的历史记录回退用预约创建时刻。
QString slotText(const services::reservation::ReservationRecord& record, const QString& format)
{
    const QDateTime start
        = record.startAtUtc.isValid() ? record.startAtUtc : record.reservation.reservedAtUtc;
    return QStringLiteral("%1—%2")
        .arg(start.toLocalTime().toString(format),
             record.reservation.expiresAtUtc.toLocalTime().toString(format));
}

// 清空布局内所有控件：deleteLater 而非直接 delete——清列表可能发生在某卡片
// 的 clicked 信号栈内（点卡片→重拉→setHistory），立即析构正在发信号的控件
// 会悬空；takeAt 只摘不删，QLayoutItem 本身需手动 delete。
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

ReservationCompletedPage::ReservationCompletedPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("reservationCompletedPage"));
    setStyleSheet(QString::fromLatin1(kCompletedPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("reservationCompletedStack"));
    rootLayout->addWidget(stack_);

    // ① 加载中。
    loadingPage_ = new QWidget(stack_);
    auto* loadingLayout = new QVBoxLayout(loadingPage_);
    auto* loadingLabel = new QLabel(tr("⏳ 正在加载历史预约…"), loadingPage_);
    loadingLabel->setObjectName(QStringLiteral("completedLoadingLabel"));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setProperty("role", QStringLiteral("secondary"));
    loadingLayout->addStretch();
    loadingLayout->addWidget(loadingLabel);
    loadingLayout->addStretch();
    stack_->addWidget(loadingPage_);

    // ② 错误态。
    errorNotice_ = new NoticePanel(QStringLiteral("⚠️"), tr("历史预约加载失败"), QString(),
                                   tr("重试"), stack_);
    errorNotice_->setObjectName(QStringLiteral("completedErrorNotice"));
    connect(static_cast<NoticePanel*>(errorNotice_), &NoticePanel::actionTriggered, this,
            &ReservationCompletedPage::retryRequested);
    stack_->addWidget(errorNotice_);

    // ③ 空态。
    emptyNotice_ = new NoticePanel(QStringLiteral("📒"), tr("暂无历史预约"),
                                   tr("结束（完成 / 取消 / 过期）的预约会归档到这里，点击卡片可查看详情。"),
                                   QString(), stack_);
    emptyNotice_->setObjectName(QStringLiteral("completedEmptyNotice"));
    stack_->addWidget(emptyNotice_);

    // ④ 历史列表（滚动容器，支持鼠标滚轮上下滚动）。
    scrollArea_ = new QScrollArea(stack_);
    scrollArea_->setObjectName(QStringLiteral("completedRecordsScroll"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listPage_ = new QWidget(scrollArea_);
    listLayout_ = new QVBoxLayout(listPage_);
    listLayout_->setContentsMargins(0, 0, 8, 0);
    listLayout_->setSpacing(10);
    scrollArea_->setWidget(listPage_);
    stack_->addWidget(scrollArea_);

    setState(PageState::Loading);
}

void ReservationCompletedPage::showLoading()
{
    setState(PageState::Loading);
}

void ReservationCompletedPage::showError(const QString& message)
{
    static_cast<NoticePanel*>(errorNotice_)->setContent(QStringLiteral("⚠️"),
                                                        tr("历史预约加载失败"), message,
                                                        tr("重试"));
    setState(PageState::Error);
}

// setHistory：整表重建而非增量对账——历史列表量小、重建幂等且免去 diff
// 逻辑出错的可能；空表自然落到 Empty 态（无需模块另行调用别的接口）。
void ReservationCompletedPage::setHistory(
    const services::reservation::ReservationList& records)
{
    records_ = records;
    clearRows();
    for (const auto& record : records_) {
        listLayout_->addWidget(createHistoryCard(record));
    }
    // 尾部弹簧：记录少时卡片保持自然高度贴在顶部，不被拉伸铺满视口。
    listLayout_->addStretch();
    setState(records_.isEmpty() ? PageState::Empty : PageState::List);
}

ReservationCompletedPage::PageState ReservationCompletedPage::viewState() const
{
    return viewState_;
}

int ReservationCompletedPage::recordCardCount() const
{
    int count = 0;
    for (int i = 0; i < listLayout_->count(); ++i) {
        const auto* item = listLayout_->itemAt(i);
        if (item != nullptr && item->widget() != nullptr
            && item->widget()->property("isHistoryReservationCard").toBool()) {
            ++count;
        }
    }
    return count;
}

bool ReservationCompletedPage::detailDialogVisible() const
{
    return detailDialog_ != nullptr && detailDialog_->isVisible();
}

QString ReservationCompletedPage::detailDialogText() const
{
    if (detailDialog_ == nullptr) {
        return QString();
    }
    const auto* text = detailDialog_->findChild<QLabel*>(
        QStringLiteral("reservationDetailDialogText"));
    return text != nullptr ? text->text() : QString();
}

void ReservationCompletedPage::setState(PageState state)
{
    viewState_ = state;
    switch (state) {
    case PageState::Loading:
        stack_->setCurrentWidget(loadingPage_);
        break;
    case PageState::Error:
        stack_->setCurrentWidget(errorNotice_);
        break;
    case PageState::Empty:
        stack_->setCurrentWidget(emptyNotice_);
        break;
    case PageState::List:
        stack_->setCurrentWidget(scrollArea_);
        break;
    }
}

void ReservationCompletedPage::clearRows()
{
    clearLayoutItems(listLayout_);
}

QWidget* ReservationCompletedPage::createHistoryCard(
    const services::reservation::ReservationRecord& record)
{
    // isHistoryReservationCard 供 recordCardCount 统计；id/status 动态属性
    // 让单测能按语义定位某张卡片，不必解析展示文案（文案属 UI，会变）。
    auto* card = new ClickableCard(listPage_);
    card->setProperty("isHistoryReservationCard", true);
    card->setProperty("reservationId", record.reservation.id);
    card->setProperty("reservationStatus", static_cast<int>(record.reservation.status));
    card->setAccessibleName(tr("%1 %2").arg(record.stationName, record.chargerCode));

    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    auto* nameLabel = new QLabel(record.stationName, card);
    nameLabel->setProperty("role", QStringLiteral("sectionTitle"));
    auto* statusTag = new StatusTag(statusTextFor(record),
                                    statusView(record.reservation.status).tone, card);
    statusTag->setObjectName(QStringLiteral("historyStatusTag"));
    titleRow->addWidget(nameLabel);
    titleRow->addStretch();
    titleRow->addWidget(statusTag);
    body->addLayout(titleRow);

    auto* infoLabel = new QLabel(
        tr("充电桩 %1 · %2 · 预约时长 %3 分钟")
            .arg(record.chargerCode,
                 record.chargerSpec.isEmpty() ? tr("充电桩") : record.chargerSpec)
            .arg(record.durationMinutes),
        card);
    infoLabel->setProperty("role", QStringLiteral("secondary"));
    infoLabel->setWordWrap(true);
    body->addWidget(infoLabel);

    auto* timeRow = new QHBoxLayout();
    auto* timeLabel = new QLabel(
        tr("预约 %1 · 时段 %2")
            .arg(record.reservation.reservedAtUtc.toLocalTime().toString(
                QStringLiteral("yyyy-MM-dd")),
                 slotText(record, QStringLiteral("HH:mm"))),
        card);
    timeLabel->setProperty("role", QStringLiteral("secondary"));
    auto* feeLabel = new QLabel(
        tr("¥%1").arg(QString::number(record.estimatedFeeCents / 100.0, 'f', 2)), card);
    feeLabel->setProperty("role", QStringLiteral("amountStrong"));
    timeRow->addWidget(timeLabel);
    timeRow->addStretch();
    timeRow->addWidget(feeLabel);
    body->addLayout(timeRow);

    auto* hintLabel = new QLabel(tr("点击查看预约详情 ›"), card);
    hintLabel->setProperty("role", QStringLiteral("caption"));
    body->addWidget(hintLabel, 0, Qt::AlignRight);

    // 按值快照再捕获：record 是模块列表存储里的引用，卡片存活期间列表随时
    // 会被 setHistory 整份重建——若捕获引用/地址，点击时必然悬空。
    const services::reservation::ReservationRecord copy = record;
    connect(card, &ClickableCard::clicked, this,
            [this, copy]() { openDetailDialog(copy); });

    return card;
}

// openDetailDialog：每次新开前先 close 旧弹窗——WA_DeleteOnClose 下用户可
// 连点不同卡片，不关就会叠出多个详情框；detailDialog_ 用 QPointer 追踪，
// 弹窗自毁后自动置空，探针（detailDialogVisible/Text）读它永不会悬空。
void ReservationCompletedPage::openDetailDialog(
    const services::reservation::ReservationRecord& record)
{
    if (detailDialog_ != nullptr) {
        detailDialog_->close();
    }
    // 详情弹窗（纯信息展示，非确认操作）：展示该预约全部字段。
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("reservationDetailDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("预约详情"));
    dialog->resize(380, 320);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("预约详情"), dialog);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    layout->addWidget(titleLabel);

    const QString text =
        tr("站点名称：%1\n充电桩：%2\n充电规格：%3\n预约时长：%4 分钟\n预约时间：%5\n"
           "预约时段：%6\n有效截止：%7\n预估费用：¥%8\n预约状态：%9")
            .arg(record.stationName,
                 record.chargerCode,
                 record.chargerSpec.isEmpty() ? tr("--") : record.chargerSpec,
                 QString::number(record.durationMinutes),
                 record.reservation.reservedAtUtc.toLocalTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm")),
                 slotText(record, QStringLiteral("MM-dd HH:mm")),
                 record.reservation.expiresAtUtc.toLocalTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm")),
                 QString::number(record.estimatedFeeCents / 100.0, 'f', 2),
                 statusTextFor(record));
    auto* body = new QLabel(text, dialog);
    body->setObjectName(QStringLiteral("reservationDetailDialogText"));
    body->setWordWrap(true);
    layout->addWidget(body);
    layout->addStretch();

    auto* closeButton = new QPushButton(tr("关闭"), dialog);
    closeButton->setObjectName(QStringLiteral("reservationDetailCloseButton"));
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(closeButton, 0, Qt::AlignRight);

    detailDialog_ = dialog;
    dialog->show();
}

} // namespace charging::client::pages::station
