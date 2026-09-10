// 头文件：通知服务对外契约（实现见同目录 notification_service.cpp）。
// 消费方：widgets 通知页/HomeShell（setNotificationService 后读 notifications()），
// QML 侧经 service_bridges 暴露；生成侧由宿主桥接预约事件调 push*。
#pragma once

#include "services/settings/settings_service.h"

#include <QDateTime>
#include <QObject>
#include <QVector>

namespace charging::client { class IRequestTransport; }

namespace charging::client::services::favorites {

// 通知消息类型：与 SettingsService::Notification 开关一一对应（int 值序对拍，
// 新类型只可尾部追加），
// “设置页关闭的类型不在通知页展示”的联动语义在 Service 层落地。
enum class NotificationType
{
    ReservationExpiryReminder, // 🔔 预约到期提醒（含迟到自动取消）
    ReservationSuccessNotice,  // ✅ 预约成功通知
    ReservationCancelNotice,   // ❌ 预约取消通知（用户主动取消）
    ChargingStopped,           // 🔌 充电结束通知（服务端通道 GET_NOTIFICATIONS）
    OrderPaid,                 // 💰 支付成功通知（服务端通道 GET_NOTIFICATIONS）
    QueueCalled,
    QueueExpired,
    RepairUpdated,
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
// 后端通知中心接口 2026-09-08 落地（GET_NOTIFICATIONS，成员 3）：注入 transport
// 后 refresh() 拉服务端通知（充电结束/支付成功）；未注入 = 纯本地通道，
// 行为与迭代 3 完全一致（widgets 端零影响）。本地演示历史与 HomeShell
// 桥接追加通道（提交成功/取消/到期·迟到）原样保留，两通道按时间合并展示。
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

    // —— 服务端通道（2026-09-08 追加，成员 3）——
    // 不注入 transport 时下面两个方法均 no-op（保持既有 widgets/测试口径）。
    void setTransport(charging::client::IRequestTransport* transport);
    // 拉取 GET_NOTIFICATIONS（pageSize=kMaxNotifications），服务端段整体替换；
    // 单飞：在途重复调用直接丢弃；失败静默保留上一次服务端段（通知页无 toast）。
    void refresh();
    bool isRefreshing() const;

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
    // 铺演示历史（ctor 与 resetForTesting 共用）：一条一型的三类预约通知。
    void seedMockHistory();
    // 本地段唯一入列口：所有 push* 与演示数据最终都收敛到这里写 items_。
    void append(NotificationType type, const QString& title, const QString& body);
    // 读设置页开关；settings_ 未注入时恒 true = 全展示（降级/独立测试口径）。
    bool enabledForType(NotificationType type) const;

    settings::SettingsService* settings_ = nullptr;
    QVector<NotificationItem> items_; // 本地通道全量（含被开关隐藏的），新在前
    qint64 nextId_ = 1;

    charging::client::IRequestTransport* transport_ = nullptr;
    QVector<NotificationItem> serverItems_; // 服务端段全量，refresh 整体替换，新在前
    bool refreshing_ = false;
    qint64 nextServerId_ = -1; // 无 id 行的合成 id：负值段，与本地正整数段防撞
};

} // namespace charging::client::services::favorites
