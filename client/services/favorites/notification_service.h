#pragma once

#include "services/settings/settings_service.h"

#include <QDateTime>
#include <QObject>
#include <QVector>

namespace charging::client::services::favorites {

// 通知消息类型：与 SettingsService::Notification 三个开关一一对应，
// “设置页关闭的类型不在通知页展示”的联动语义在 Service 层落地。
enum class NotificationType
{
    ReservationExpiryReminder, // 🔔 预约到期提醒（含迟到自动取消）
    ReservationSuccessNotice,  // ✅ 预约成功通知
    ReservationCancelNotice,   // ❌ 预约取消通知（用户主动取消）
};

// 一条站内通知（标题/内容/时间——迭代 3 通知页展示口径）。
struct NotificationItem
{
    qint64 id = 0;
    NotificationType type = NotificationType::ReservationSuccessNotice;
    QString title;
    QString body;
    QDateTime createdAtUtc;
};

// 通知服务（成员 2，迭代 3）：消息通知页的数据层。
// 与收藏服务同属“迭代 3 个人中心域”（同库 charging_client_favorites_services，
// 命名空间共用 services::favorites）。
//
// 后端通知中心接口尚未定义（协议属成员 1/3 领域）——当前为客户端 Service
// 模拟：构造时按预约业务口径生成演示历史，运行期由 HomeShell 桥接
// ReservationService 信号（提交成功/取消/到期·迟到）实时追加；TODO(contract)：
// 接口就绪后仅替换数据源，notifications()/notificationsChanged() 形状不变。
//
// 开关联动：setSettingsService() 注入后，notifications() 只返回“对应开关
// 开启”的类型；设置页 notificationsChanged 原样转发，页面即时重渲染。
// 未注入设置服务 = 全展示（独立测试/降级口径）。
class NotificationService final : public QObject
{
    Q_OBJECT

public:
    explicit NotificationService(QObject* parent = nullptr);

    void setSettingsService(settings::SettingsService* settings);

    // 演示/测试：清空并重新生成模拟历史。
    void resetForTesting();

    // 当前可见通知（新→旧；已按设置开关过滤）。
    QVector<NotificationItem> notifications() const;
    int visibleCount() const;

    // —— 生成通道（HomeShell 桥接 ReservationService 信号调用）——
    // stationName/chargerCode/plate 为展示上下文，由调用方从预约记录取。
    void pushReservationSuccess(const QString& stationName, const QString& chargerCode,
                                const QString& vehiclePlate, const QDateTime& startAtUtc);
    void pushReservationCancelled(const QString& stationName, const QString& chargerCode);
    // 到期提醒：late=true 表示“迟到超 15 分钟被自动取消”口径（预约到期
    // 提醒类型），late=false 为倒计时归零“已过期”提醒。
    void pushReservationExpired(const QString& stationName, const QString& chargerCode,
                                bool late);

    static QString typeTitle(NotificationType type); // 标题前缀（列表/测试用）

signals:
    void notificationsChanged();

private:
    void seedMockHistory();
    void append(NotificationType type, const QString& title, const QString& body);
    bool enabledForType(NotificationType type) const;

    settings::SettingsService* settings_ = nullptr;
    QVector<NotificationItem> items_; // 全量（含被开关隐藏的），新在前
    qint64 nextId_ = 1;
};

} // namespace charging::client::services::favorites
