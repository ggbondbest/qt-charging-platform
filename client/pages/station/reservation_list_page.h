#pragma once

#include "charging/common/model/models.h"
#include "services/reservation/reservation_service.h"

#include <QWidget>

class QLabel;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace charging::client {
class NoticePanel;
}

namespace charging::client::pages::station {

// 我的预约记录页（成员 2，任务 #17）：从“我的”页入口进入，挂在 HomeShell
// 路由栈（复用全局顶部导航 + 底部 Tab，页面本身不实现导航）。
//
// 每条记录卡片：站点名称、充电桩编号、预约时间、预约时长、预约状态
// （预约中 / 已完成 / 已取消 / 已过期）。“预约中”提供【取消预约】按钮，
// 其余状态按钮置灰不可操作；取消成功后刷新列表体现状态变化。
//
// 边界状态全覆盖：加载中 / 空记录（友好提示）/ 接口或网络错误（重试）；
// 未登录拦截由 HomeShell 负责（本页面只在登录态路由进入）。数据仅经
// ReservationService 获取，模拟 ↔ 真实切换 UI 零改动。
class ReservationListPage final : public QWidget
{
    Q_OBJECT

public:
    enum class State
    {
        Loading,
        Error,
        Empty,
        List,
    };

    explicit ReservationListPage(QWidget* parent = nullptr);

    // 非拥有：与详情页共用同一预约服务实例（HomeShell 注入）。
    void setService(charging::client::services::reservation::ReservationService* service);
    charging::client::services::reservation::ReservationService* service() const;

    // 拉取记录（进入路由时由 HomeShell 调用）。
    void refresh();

    // 测试探针。
    State viewState() const;
    int recordCardCount() const;

signals:
    // 取消失败原因已由页内错误条展示；信号供宿主/测试观察。
    void cancelFailed(const QString& message);

private:
    void setState(State state);
    void rebuildCards();
    QWidget* createRecordCard(const services::reservation::ReservationRecord& record);
    void handleListStarted();
    void handleListSucceeded(
        const services::reservation::ReservationList& records);
    void handleListFailed(const QString& message);
    void handleCancelSucceeded(qint64 reservationId);
    void handleCancelFailed(const QString& message);
    void clearRows();

    services::reservation::ReservationService* service_ = nullptr; // not owned
    State viewState_ = State::Loading;
    services::reservation::ReservationList records_;

    QStackedWidget* pageStack_ = nullptr;
    QWidget* loadingPage_ = nullptr;
    NoticePanel* errorNotice_ = nullptr;
    NoticePanel* emptyNotice_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QWidget* listPage_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    QLabel* cancelErrorLabel_ = nullptr;
};

} // namespace charging::client::pages::station
