#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

namespace charging::client::services::reservation {
class ReservationService;
struct ReservationRecord;
} // namespace charging::client::services::reservation

namespace charging::client::pages::station {

// 预约确认独立页面（成员 2，任务 #17 迭代）：替换原弹窗式确认预约。
//
// 路由入口：站点详情页充电桩卡片的“预约”按钮——仅在已登录且**无未结束
// 预约**时进入（拦截逻辑由详情页/宿主完成，Service 层兜底二次校验）。
//
// 页面内容 = 站点名称 / 充电桩编号 / 充电类型与功率 / 预约时长下拉
// （30/60/90/120 分钟，默认 60）/ 预估费用（电价 × 时长即时联动）；
// 底部按钮：【关闭】返回站点详情页（交宿主路由）、【确认预约】经
// ReservationService 双通道提交（模拟 ↔ 真实 UI 零改动）：
// - 提交中：按钮禁用并显示“提交中…”（loading 态）；
// - 成功：发 confirmed(record) 信号，宿主自动路由至【预约订单】页；
// - 失败：红色展示失败原因（桩被抢占 / 已有未结束预约 / 参数非法 /
//   网络错误），停留在本页可修改后重试。
//
// 导航复用任务 #2 全局顶部/底部外壳，本页不实现任何导航栏代码；
// 长内容置于 QScrollArea，鼠标滚轮上下滚动。
class ReservationConfirmPage final : public QWidget
{
    Q_OBJECT

public:
    enum class PageState
    {
        Idle,       // 可编辑表单
        Submitting, // 提交中（loading）
    };

    explicit ReservationConfirmPage(QWidget* parent = nullptr);

    // 非拥有：由 HomeShell 统一注入（与详情页/预约模块同一实例）。
    void setService(charging::client::services::reservation::ReservationService* service);

    // 路由入口：刷新预约上下文并复位表单/提示。distanceMeters 为展示用
    // 虚拟导航距离（预约订单页左栏，预留对接后续导航模块）。
    void openContext(const charging::model::Station& station,
                     const charging::model::Charger& charger, int distanceMeters);

    // 测试探针。
    PageState pageState() const;
    int selectedMinutes() const;
    QString estimatedFeeText() const;
    QString messageText() const;

signals:
    // 【关闭】：宿主路由返回站点详情页。
    void closeRequested();
    // 预约提交成功：宿主刷新桩状态并自动路由至【预约订单】页面。
    void confirmed(const charging::client::services::reservation::ReservationRecord& record);

private:
    void updateEstimatedFee();
    void handleSubmit();
    void handleSubmitStarted(qint64 chargerId);
    void handleSubmitSucceeded(
        const charging::client::services::reservation::ReservationRecord& record);
    void handleSubmitFailed(const QString& reason);

    charging::client::services::reservation::ReservationService* service_ = nullptr;
    charging::model::Station station_;
    charging::model::Charger charger_;
    int distanceMeters_ = -1;
    bool submitting_ = false;

    QLabel* stationNameLabel_ = nullptr;
    QLabel* chargerCodeLabel_ = nullptr;
    QLabel* chargerSpecLabel_ = nullptr;
    QComboBox* durationComboBox_ = nullptr;
    QLabel* feeLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
};

} // namespace charging::client::pages::station
