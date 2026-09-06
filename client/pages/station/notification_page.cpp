#include "pages/station/notification_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/notice_panel.h"
#include "pages/station/platform_theme.h"

#include <QLabel>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

using services::favorites::NotificationItem;
using services::favorites::NotificationService;

// 页面局部样式：token 与全局规范同源，仅本页生效。
const char* kNotificationPageStyleSheet = R"(
QWidget#notificationPage {
    background: #F7F9FB;
}
QLabel#notificationPageTitle {
    color: #1F2937;
    font-size: 16px;
    font-weight: 700;
}
QLabel#notificationPageCaption {
    color: #9AA5B1;
    font-size: 11px;
}
QLabel#notificationTimeLabel {
    color: #9CA3AF;
    font-size: 11px;
}
)";

void clearLayoutItems(QVBoxLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

NotificationPage::NotificationPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("notificationPage"));
    setStyleSheet(QString::fromLatin1(kNotificationPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("消息通知"), this);
    titleLabel->setObjectName(QStringLiteral("notificationPageTitle"));
    rootLayout->addWidget(titleLabel);

    auto* caption = new QLabel(
        tr("展示预约相关的业务消息；类型开关位于「我的-设置-通知与提醒」，"
           "关闭的类型不在此展示。"),
        this);
    caption->setObjectName(QStringLiteral("notificationPageCaption"));
    caption->setWordWrap(true);
    captionLabel_ = caption;
    rootLayout->addWidget(caption);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("notificationStack"));
    rootLayout->addWidget(stack_, 1);

    emptyNotice_ = new NoticePanel(QStringLiteral("🔔"), tr("暂无通知"),
                                   tr("开启新的预约后，成功/取消/到期消息会出现在这里；"
                                      "也可到设置页检查“通知与提醒”开关。"),
                                   QString(), stack_);
    stack_->addWidget(emptyNotice_);

    // 长内容滚动容器：鼠标滚轮上下滚动（规格通用要求）。
    auto* scroll = new QScrollArea(stack_);
    scroll->setObjectName(QStringLiteral("notificationScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listPage_ = new QWidget(scroll);
    listLayout_ = new QVBoxLayout(listPage_);
    listLayout_->setContentsMargins(0, 0, 8, 0);
    listLayout_->setSpacing(10);
    scroll->setWidget(listPage_);
    stack_->addWidget(scroll);

    stack_->setCurrentIndex(0);
}

void NotificationPage::setNotificationService(NotificationService* service)
{
    if (service_ == service) {
        return;
    }
    if (service_ != nullptr) {
        disconnect(service_, nullptr, this, nullptr);
    }
    service_ = service;
    if (service_ != nullptr) {
        // 新消息 / 设置开关变化都会汇入 notificationsChanged → 即时重渲染。
        connect(service_, &NotificationService::notificationsChanged, this,
                &NotificationPage::refresh);
    }
    refresh();
}

void NotificationPage::refresh()
{
    const QVector<NotificationItem> items
        = service_ != nullptr ? service_->notifications() : QVector<NotificationItem>{};

    clearLayoutItems(listLayout_);
    for (const NotificationItem& item : items) {
        listLayout_->addWidget(createNotificationCard(item));
    }
    listLayout_->addStretch();

    stack_->setCurrentIndex(items.isEmpty() ? 0 : 1);
}

QWidget* NotificationPage::createNotificationCard(const NotificationItem& item)
{
    auto* card = new Card(listPage_);
    card->setProperty("isNotificationCard", true);
    auto* body = card->bodyLayout();

    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(item.title, card);
    titleLabel->setProperty("role", QStringLiteral("sectionTitle"));
    titleLabel->setWordWrap(true);
    auto* timeLabel = new QLabel(
        item.createdAtUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")), card);
    timeLabel->setObjectName(QStringLiteral("notificationTimeLabel"));
    headerRow->addWidget(titleLabel, 1);
    headerRow->addWidget(timeLabel, 0, Qt::AlignTop);
    body->addLayout(headerRow);

    auto* bodyLabel = new QLabel(item.body, card);
    bodyLabel->setProperty("role", QStringLiteral("secondary"));
    bodyLabel->setWordWrap(true);
    body->addWidget(bodyLabel);

    return card;
}

int NotificationPage::notificationCardCount() const
{
    int count = 0;
    for (int i = 0; i < listLayout_->count(); ++i) {
        const QLayoutItem* item = listLayout_->itemAt(i);
        if (item != nullptr && item->widget() != nullptr
            && item->widget()->property("isNotificationCard").toBool()) {
            ++count;
        }
    }
    return count;
}

bool NotificationPage::emptyStateVisible() const
{
    return stack_->currentWidget() == emptyNotice_ && emptyNotice_->isVisible();
}

} // namespace charging::client::pages::station
