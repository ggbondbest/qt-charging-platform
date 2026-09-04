#pragma once

#include "services/reservation/reservation_service.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace charging::client::pages::station {

// 预约订单页（成员 2，任务 #17 迭代）：预约模块二级 Tab 之一，展示用户
// **进行中的预约**（业务约束保证至多一条）。
//
// 左-中-右三栏横向布局：
// - 左栏：距离模块（距预约桩的虚拟数据，预留对接后续导航功能）；
// - 中栏：预约信息 + **倒计时**，每秒自动刷新一次，按剩余时长切换字体
//   颜色：>30 分钟绿色 / 5~30 分钟黄色 / <5 分钟红色；倒计时归零自动
//   调用 Service 流转预约状态（预约中 → 已过期）并刷新页面；
// - 右栏：汽车电量占位模块（虚拟占位，业务暂不实现）。
// 中栏含【取消预约】按钮：取消成功由宿主跳转【已完成的预约】页。
//
// 边界状态：无进行中预约 → 友好空提示；加载中 / 接口异常态由模块驱动。
// 内容置于 QScrollArea，鼠标滚轮上下滚动。
class ReservationOrderPage final : public QWidget
{
    Q_OBJECT

public:
    enum class PageState
    {
        Loading, // 列表拉取中
        Error,   // 接口/网络异常（含未注入服务）
        Empty,   // 无进行中预约（友好提示）
        Active,  // 有进行中预约（三栏 + 倒计时）
    };

    explicit ReservationOrderPage(QWidget* parent = nullptr);

    // 非拥有：取消操作经服务双通道提交（模拟 ↔ 真实 UI 零改动）。
    void setService(charging::client::services::reservation::ReservationService* service);

    // 模块分发列表结果：设置/清空进行中预约（null 指针 → Empty）。
    void setActiveReservation(const charging::client::services::reservation::ReservationRecord* record);
    // 模块驱动加载/错误态。
    void showLoading();
    void showError(const QString& message);

    // 测试探针。
    PageState viewState() const;
    QString countdownText() const;
    // 倒计时色调探针："green" / "yellow" / "red" / ""（无进行中预约）。
    QString countdownColorRole() const;
    QString distanceText() const;
    QString batteryText() const;

signals:
    // 空态引导按钮：宿主路由去“找站”Tab。
    void findStationRequested();

private:
    void applyCountdown();
    void handleCancelClicked();
    void handleCancelStarted(qint64 reservationId);
    void handleCancelFailed(const QString& message);

    charging::client::services::reservation::ReservationService* service_ = nullptr;
    QTimer* countdownTimer_ = nullptr;
    charging::client::services::reservation::ReservationRecord active_;
    bool hasActive_ = false;
    bool cancelling_ = false;
    PageState viewState_ = PageState::Loading;

    QStackedWidget* stack_ = nullptr;
    QWidget* loadingPage_ = nullptr;
    QWidget* errorNotice_ = nullptr;
    QWidget* emptyNotice_ = nullptr;
    QLabel* countdownLabel_ = nullptr;
    QLabel* expiresAtLabel_ = nullptr;
    QLabel* activeInfoLabel_ = nullptr;
    QLabel* distanceLabel_ = nullptr;
    QLabel* batteryLabel_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
};

} // namespace charging::client::pages::station
