#include "services/favorites/notification_service.h"

#include <QDateTime>

namespace charging::client::services::favorites {

namespace {

// 列表上限：模拟通道下防止桥接风暴把内存撑大（真实通道改为服务端分页）。
constexpr int kMaxNotifications = 50;

// 演示历史的时间偏移（分钟）：一条一型，覆盖三类通知的展示样式。
constexpr int kSeedOffsetsMinutes[3] = {14, 95, 1520};

} // namespace

NotificationService::NotificationService(QObject* parent) : QObject(parent)
{
    seedMockHistory();
}

void NotificationService::setSettingsService(settings::SettingsService* settings)
{
    if (settings_ == settings) {
        return;
    }
    settings_ = settings;
    if (settings_ != nullptr) {
        // 开关联动：设置页任意开关变化 → 本页可见集变化，原样转发。
        connect(settings_, &settings::SettingsService::notificationsChanged, this,
                &NotificationService::notificationsChanged);
    }
}

void NotificationService::resetForTesting()
{
    items_.clear();
    nextId_ = 1;
    seedMockHistory();
    emit notificationsChanged();
}

bool NotificationService::enabledForType(NotificationType type) const
{
    if (settings_ == nullptr) {
        return true; // 未注入 = 全展示（独立测试/降级口径）
    }
    // NotificationType 与 SettingsService::Notification 枚举值一一对应。
    return settings_->notificationEnabled(
        static_cast<settings::SettingsService::Notification>(static_cast<int>(type)));
}

QVector<NotificationItem> NotificationService::notifications() const
{
    QVector<NotificationItem> visible;
    visible.reserve(items_.size());
    for (const NotificationItem& item : items_) {
        if (enabledForType(item.type)) {
            visible.append(item);
        }
    }
    return visible;
}

int NotificationService::visibleCount() const
{
    return notifications().size();
}

QString NotificationService::typeTitle(NotificationType type)
{
    switch (type) {
    case NotificationType::ReservationExpiryReminder:
        return QStringLiteral("🔔 预约到期提醒");
    case NotificationType::ReservationSuccessNotice:
        return QStringLiteral("✅ 预约成功通知");
    case NotificationType::ReservationCancelNotice:
        return QStringLiteral("❌ 预约取消通知");
    }
    return QStringLiteral("📣 系统通知");
}

void NotificationService::seedMockHistory()
{
    // 一条一型，覆盖三类通知的展示样式（时间倒序入列，最新在前）。
    auto seed = [this](NotificationType type, const QString& body, int minutesAgo) {
        NotificationItem item;
        item.id = nextId_++;
        item.type = type;
        item.title = typeTitle(type);
        item.body = body;
        item.createdAtUtc = QDateTime::currentDateTimeUtc().addSecs(-minutesAgo * 60);
        items_.prepend(item); // 新在前
    };
    seed(NotificationType::ReservationCancelNotice,
         QStringLiteral("西丽湖临时站 · SZ-XLH-06-02 的预约已取消。"),
         kSeedOffsetsMinutes[2]);
    seed(NotificationType::ReservationExpiryReminder,
         QStringLiteral("南山智造充电站 · SZ-NSZ-03-01 预约时段已过，期待下次光临。"),
         kSeedOffsetsMinutes[1]);
    seed(NotificationType::ReservationSuccessNotice,
         QStringLiteral("科技园充电驿站 · SZ-KEY-01-03 预约成功，请按时前往。"),
         kSeedOffsetsMinutes[0]);
}

void NotificationService::append(NotificationType type, const QString& title,
                                 const QString& body)
{
    NotificationItem item;
    item.id = nextId_++;
    item.type = type;
    item.title = title;
    item.body = body;
    item.createdAtUtc = QDateTime::currentDateTimeUtc();
    items_.prepend(item);
    while (items_.size() > kMaxNotifications) {
        items_.removeLast(); // 只裁剪展示列表，不影响 id 单调
    }
    // 类型被开关隐藏时仍然入列（重新打开开关后可见），但集合无变化时
    // 不空发信号打扰页面：全量列表变化即视为可见集可能变化，保守转发。
    emit notificationsChanged();
}

void NotificationService::pushReservationSuccess(const QString& stationName,
                                                 const QString& chargerCode,
                                                 const QString& vehiclePlate,
                                                 const QDateTime& startAtUtc)
{
    QString body = tr("%1 · %2 预约成功").arg(stationName, chargerCode);
    if (startAtUtc.isValid()) {
        body += tr("，%1 开始").arg(startAtUtc.toLocalTime().toString(QStringLiteral("HH:mm")));
    }
    if (!vehiclePlate.isEmpty()) {
        body += tr("（%1）").arg(vehiclePlate);
    }
    body += tr("，请按时前往。");
    append(NotificationType::ReservationSuccessNotice,
           typeTitle(NotificationType::ReservationSuccessNotice), body);
}

void NotificationService::pushReservationCancelled(const QString& stationName,
                                                   const QString& chargerCode)
{
    append(NotificationType::ReservationCancelNotice,
           typeTitle(NotificationType::ReservationCancelNotice),
           tr("%1 · %2 的预约已取消。").arg(stationName, chargerCode));
}

void NotificationService::pushReservationExpired(const QString& stationName,
                                                 const QString& chargerCode, bool late)
{
    const QString body = late
        ? tr("%1 · %2 超过开始时间 15 分钟未到场，预约已自动取消。")
              .arg(stationName, chargerCode)
        : tr("%1 · %2 预约时段已结束，期待下次光临。").arg(stationName, chargerCode);
    append(NotificationType::ReservationExpiryReminder,
           typeTitle(NotificationType::ReservationExpiryReminder), body);
}

} // namespace charging::client::services::favorites
