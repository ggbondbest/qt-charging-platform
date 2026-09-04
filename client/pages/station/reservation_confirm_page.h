#pragma once

#include "charging/common/model/models.h"

#include <QDateTime>
#include <QWidget>

class QComboBox;
class QDateTimeEdit;
class QLabel;
class QPushButton;

namespace charging::client::services::reservation {
class ReservationService;
struct ReservationRecord;
} // namespace charging::client::services::reservation

namespace charging::client::services::map {
class MapGeoService;
struct DistanceElement;
} // namespace charging::client::services::map

namespace charging::client::services::settings {
class SettingsService;
}

namespace charging::client::pages::station {

// 预约确认独立页面（成员 2，任务 #17 二次迭代改版）：
// 从“选时长下拉”升级为“选时间段 + 选车辆”。
//
// 路由入口：站点详情页充电桩卡片的“预约”按钮——仅在已登录、已添加车辆
// 且名额未满时进入（拦截逻辑由详情页/宿主完成，Service 层兜底二次校验）。
//
// 页面内容 = 站点名称 / 充电桩编号 / 充电类型与功率 / 预约车辆下拉
// （来自设置-车辆管理，默认选中默认车辆）/ 开始·结束时间（QDateTimeEdit）/
// “✨ 使用系统推荐时段”按钮（基于虚拟导航距离的模拟行驶时长估算，真实
// 地图 API 就绪后仅替换估算，页面不变）/ 预估费用（电价 × 时段时长即时
// 联动）。业务约束：单段不超过 45 分钟（超出时红色行内提示并禁用提交，
// Service 层兜底）。底部按钮：【关闭】返回站点详情页；【确认预约】经
// ReservationService 双通道提交：
// - 提交中：按钮禁用并显示“提交中…”（loading 态）；
// - 成功：发 succeeded(record)，宿主弹“是否现在前往充电？”——去充电 →
//   导航页，稍后再说 → 【预约订单】页；
// - 失败：红色展示失败原因（桩被抢占 / 名额占用 / 参数非法 / 网络错误），
//   停留在本页可修改后重试。
//
// 导航复用全局顶部/底部外壳，本页不实现任何导航栏代码；
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
    // 车辆下拉数据源（设置-车辆管理）；车辆增删改实时联动本下拉。
    void setSettingsService(charging::client::services::settings::SettingsService* settings);
    // 腾讯地图服务（成员 2 地图接入）：注入且 key 可用时，进入本页在
    // 模拟推荐之外再请求距离矩阵，用真实行驶距离/时长升级推荐时段；
    // 接口异常 Toast 提示并保持模拟口径。未注入 = 纯模拟（现状行为）。
    void setMapService(charging::client::services::map::MapGeoService* mapService);

    // 路由入口：刷新预约上下文并复位表单/提示。distanceMeters 为展示用
    // 虚拟导航距离（推荐时段估算与订单页左栏共用，对接导航模块）。
    void openContext(const charging::model::Station& station,
                     const charging::model::Charger& charger, int distanceMeters);

    // 测试探针。
    PageState pageState() const;
    int selectedMinutes() const;
    qint64 selectedVehicleId() const;
    QDateTime startUtc() const;
    QDateTime endUtc() const;
    QString estimatedFeeText() const;
    QString messageText() const;
    QString recommendedSlotText() const;

signals:
    // 【关闭】：宿主路由返回站点详情页。
    void closeRequested();
    // 预约提交成功：宿主弹“是否现在前往充电？”后路由（导航页 / 预约订单页）。
    void succeeded(const charging::client::services::reservation::ReservationRecord& record);

private:
    void refreshVehicles();
    void applyRecommendedSlot();
    void refreshRecommendedButton();
    void handleMatrixResult(quint64 requestId,
                            const QVector<charging::client::services::map::DistanceElement>& elements);
    void handleMatrixFailure(quint64 requestId, const QString& message);
    void updateSlotValidity();
    void handleSubmit();
    void handleSubmitStarted(qint64 chargerId);
    void handleSubmitSucceeded(
        const charging::client::services::reservation::ReservationRecord& record);
    void handleSubmitFailed(const QString& reason);
    void resetSubmitButton();

    charging::client::services::reservation::ReservationService* service_ = nullptr;
    charging::client::services::settings::SettingsService* settings_ = nullptr;
    charging::client::services::map::MapGeoService* mapService_ = nullptr;
    quint64 mapGeneration_ = 0;    // 过期矩阵响应过滤（快速切换站点时旧响应后到）
    bool userEditedSlot_ = false;  // 用户手动改过起止时间：真实结果不覆盖其编辑
    bool applyingSlot_ = false;    // 程序写入 QDateTimeEdit 时屏蔽 userEdited 置位
    QString recommendedBaseText_;  // 推荐按钮基础文案（“更新中”后缀之外的部分）
    charging::model::Station station_;
    charging::model::Charger charger_;
    int distanceMeters_ = -1;
    bool submitting_ = false;

    QLabel* stationNameLabel_ = nullptr;
    QLabel* chargerCodeLabel_ = nullptr;
    QLabel* chargerSpecLabel_ = nullptr;
    QComboBox* vehicleComboBox_ = nullptr;
    QDateTimeEdit* startEdit_ = nullptr;
    QDateTimeEdit* endEdit_ = nullptr;
    QPushButton* recommendedButton_ = nullptr;
    QLabel* feeLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
};

} // namespace charging::client::pages::station
