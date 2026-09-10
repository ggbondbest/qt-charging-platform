// 文件职责：消息通知页的类声明（迭代 3，与 notification_page.cpp 成对）。
// 使用方：HomeShell 构造本页面并注入壳内 NotificationService 单实例，作为
// 内容栈路由页挂入（顶部铃铛进入，登录门禁在壳侧 openNotifications 拦截）。
// 数据流向：纯本机同步数据源（页面 ← NotificationService 内存列表 + QSettings
// 开关过滤），不经 TCP 契约；本页只读展示，无写回通道。
#pragma once

#include "services/favorites/notification_service.h"

#include <QWidget>

class QLabel;
class QStackedWidget;
class QVBoxLayout;

namespace charging::client {
class NoticePanel;
}

namespace charging::client::pages::station {

// 消息通知页（成员 2，迭代 3）：找站页顶部铃铛进入的路由页，复用全局
// 顶部导航 + 底部 Tab（壳层口径）。自上而下：标题 → 通知卡列表（标题/内容/
// 时间，新→旧）→ 空态。
//
// 数据层 NotificationService（HomeShell 注入，与预约信号桥接同实例）：
// 列表已由服务按设置页“通知与提醒”三开关过滤——关闭的类型不在本页展示；
// notificationsChanged（新通知/开关切换）触发整页重渲染。
// 本地服务为同步数据源：无加载/网络异常态；空数据展示引导空态（规格口径）。
class NotificationPage final : public QWidget
{
    Q_OBJECT

public:
    explicit NotificationPage(QWidget* parent = nullptr);

    // 注入通知数据服务（HomeShell 壳内单实例）：同实例重复注入幂等短路；
    // 换实例先摘净旧对象的信号再重接，避免两路服务同时驱动重渲染。
    void setNotificationService(
        charging::client::services::favorites::NotificationService* service);

    // 依据服务当前可见集重建列表（壳层路由进入时也会调用）。
    void refresh();

    // 测试探针。
    int notificationCardCount() const;
    bool emptyStateVisible() const;

private:
    // 单条通知卡工厂：标题 + UTC→本地时间一行、正文一行，样式走全局 role。
    QWidget* createNotificationCard(
        const charging::client::services::favorites::NotificationItem& item);

    charging::client::services::favorites::NotificationService* service_ = nullptr; // not owned

    // ---- 展示骨架：stack_ 两态（0=空态引导 emptyNotice_，1=滚动列表
    // listPage_/listLayout_）；captionLabel_ 为副标题引用留存（当前只写不读） ----
    QVBoxLayout* listLayout_ = nullptr;
    QWidget* listPage_ = nullptr;
    NoticePanel* emptyNotice_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* captionLabel_ = nullptr;
};

} // namespace charging::client::pages::station
