#include "services/favorites/notification_service.h"

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/protocol/protocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace charging::client::services::favorites {

namespace {

// 列表上限：模拟通道下防止桥接风暴把内存撑大（真实通道改为服务端分页）。
constexpr int kMaxNotifications = 50;

// 演示历史的时间偏移（分钟）：一条一型，覆盖三类通知的展示样式。
constexpr int kSeedOffsetsMinutes[3] = {14, 95, 1520};

// 服务端 type 词（协议小写串）→ 本地枚举；未知词返回 false（防御性丢弃，
// 服务端 schema CHECK 目前放行 charging_stopped/order_paid/reservation_expiry_reminder）。
bool typeFromServerWord(const QString& word, NotificationType* out)
{
    if (word == QLatin1String("reservation_expiry_reminder")) {
        *out = NotificationType::ReservationExpiryReminder;
    } else if (word == QLatin1String("reservation_success_notice")) {
        *out = NotificationType::ReservationSuccessNotice;
    } else if (word == QLatin1String("reservation_cancel_notice")) {
        *out = NotificationType::ReservationCancelNotice;
    } else if (word == QLatin1String("charging_stopped")) {
        *out = NotificationType::ChargingStopped;
    } else if (word == QLatin1String("order_paid")) {
        *out = NotificationType::OrderPaid;
    } else {
        return false;
    }
    return true;
}

// createdAtUtc 双形态兼容：数字 = epoch ms（CouponPage 同款约定），字符串 =
// ISODateWithMs/ISODate（服务端与 mock 输出均为 UTC 无后缀，按 UTC 解释）。
QDateTime parseCreatedAt(const QJsonValue& value)
{
    if (value.isDouble()) {
        return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(value.toDouble()), Qt::UTC);
    }
    const QString text = value.toString();
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(text, Qt::ISODate);
    }
    if (parsed.isValid()) {
        parsed.setTimeSpec(Qt::UTC);
    }
    return parsed;
}

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

void NotificationService::setTransport(charging::client::IRequestTransport* transport)
{
    transport_ = transport;
}

bool NotificationService::isRefreshing() const
{
    return refreshing_;
}

void NotificationService::refresh()
{
    if (transport_ == nullptr || refreshing_) {
        return; // 未注入 = 纯本地通道；在途 = 单飞丢弃
    }
    refreshing_ = true;
    // sendFor：回调经 QPointer 守卫，本服务先销毁则迟到响应被安全丢弃。
    transport_->sendFor(this,
                        QString::fromLatin1(charging::protocol::request_type::kGetNotifications),
                        {{QStringLiteral("pageSize"), kMaxNotifications}},
                        [this](bool ok, const QJsonObject& data,
                               const charging::protocol::ProtocolError&) {
                            refreshing_ = false;
                            if (!ok) {
                                return; // 失败静默：保留上一次服务端段，本地通道不受影响
                            }
                            QVector<NotificationItem> rows;
                            const QJsonArray notifications =
                                data.value(QStringLiteral("notifications")).toArray();
                            for (const QJsonValue& value : notifications) {
                                const QJsonObject row = value.toObject();
                                NotificationItem item;
                                if (!typeFromServerWord(
                                        row.value(QStringLiteral("type")).toString(), &item.type)) {
                                    continue; // 未知类型防御性丢弃
                                }
                                const QString id = row.value(QStringLiteral("id")).toString();
                                item.id = id.isEmpty() ? nextServerId_-- : id.toLongLong();
                                item.title = row.value(QStringLiteral("title")).toString();
                                item.body = row.value(QStringLiteral("body")).toString();
                                item.createdAtUtc =
                                    parseCreatedAt(row.value(QStringLiteral("createdAtUtc")));
                                rows.append(item); // 服务端已新→旧，按序入列
                            }
                            serverItems_ = rows;
                            emit notificationsChanged();
                        });
}

void NotificationService::resetForTesting()
{
    items_.clear();
    serverItems_.clear();
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
    // 本地段独有（未注入 transport / 尚未拉到数据）时走原路径，零额外开销。
    if (serverItems_.isEmpty()) {
        QVector<NotificationItem> visible;
        visible.reserve(items_.size());
        for (const NotificationItem& item : items_) {
            if (enabledForType(item.type)) {
                visible.append(item);
            }
        }
        return visible;
    }
    // 双通道合并：本地 push 段 + 服务端段按 createdAtUtc 倒序（stable 保各段内
    // 原序），(type, body, 秒) 键去重防未来 reservation 类型双发，再开关过滤。
    QVector<NotificationItem> merged;
    merged.reserve(items_.size() + serverItems_.size());
    merged += items_;
    merged += serverItems_;
    std::stable_sort(merged.begin(), merged.end(),
                     [](const NotificationItem& a, const NotificationItem& b) {
                         return a.createdAtUtc > b.createdAtUtc;
                     });
    QVector<NotificationItem> visible;
    visible.reserve(merged.size());
    QSet<QString> seen;
    for (const NotificationItem& item : merged) {
        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(static_cast<int>(item.type))
                                .arg(item.body)
                                .arg(item.createdAtUtc.toSecsSinceEpoch());
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
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

// 标题纯文字：类型图形由 QML 通知页行首的线稿图标承担（emoji 前缀已剥，
// 避免文本图标与 glyph 双重表意）。
QString NotificationService::typeTitle(NotificationType type)
{
    switch (type) {
    case NotificationType::ReservationExpiryReminder:
        return QStringLiteral("预约到期提醒");
    case NotificationType::ReservationSuccessNotice:
        return QStringLiteral("预约成功通知");
    case NotificationType::ReservationCancelNotice:
        return QStringLiteral("预约取消通知");
    case NotificationType::ChargingStopped:
        return QStringLiteral("充电结束通知");
    case NotificationType::OrderPaid:
        return QStringLiteral("支付成功通知");
    }
    return QStringLiteral("系统通知");
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
