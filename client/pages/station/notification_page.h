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

    void setNotificationService(
        charging::client::services::favorites::NotificationService* service);

    // 依据服务当前可见集重建列表（壳层路由进入时也会调用）。
    void refresh();

    // 测试探针。
    int notificationCardCount() const;
    bool emptyStateVisible() const;

private:
    QWidget* createNotificationCard(
        const charging::client::services::favorites::NotificationItem& item);

    charging::client::services::favorites::NotificationService* service_ = nullptr; // not owned

    QVBoxLayout* listLayout_ = nullptr;
    QWidget* listPage_ = nullptr;
    NoticePanel* emptyNotice_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* captionLabel_ = nullptr;
};

} // namespace charging::client::pages::station
