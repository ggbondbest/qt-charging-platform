#pragma once

#include "charging/common/model/models.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;

namespace charging::client::services::reservation {
class ReservationService;
struct ReservationRecord;
}

namespace charging::client::pages::station {

// 预约弹窗（成员 2，任务 #17）：挂载在任务 #12 站点详情页的预约入口上。
//
// 表单：预约时长下拉 + 桩编号 / 站点名称 / 预估费用展示；提交经
// ReservationService 双通道（模拟 ↔ 真实 UI 零改动）：
// - 提交中：按钮禁用并显示“提交中…”（loading 态）；
// - 成功：绿色成功提示，短暂停留后自动关闭（宿主收到 reserved 后刷新桩状态）；
// - 失败：红色展示失败原因（桩被抢占 / 预约冲突 / 参数非法 / 网络错误），
//   弹窗保持打开、按钮恢复，支持手动关闭。
class ReservationDialog final : public QDialog
{
    Q_OBJECT

public:
    ReservationDialog(const charging::model::Station& station,
                      const charging::model::Charger& charger, QWidget* parent = nullptr);

    // 非拥有：由 HomeShell 统一注入（与详情页/记录页同一实例）。
    void setService(charging::client::services::reservation::ReservationService* service);

    // 测试探针。
    int selectedMinutes() const;
    QString estimatedFeeText() const;
    QString messageText() const;

signals:
    // 预约提交成功（弹窗展示成功提示后自动 accept）。
    void reserved(qint64 chargerId);

private:
    void updateEstimatedFee();
    void handleSubmit();
    void handleSubmitStarted(qint64 chargerId);
    void handleSubmitSucceeded(
        const charging::client::services::reservation::ReservationRecord& record);
    void handleSubmitFailed(const QString& reason);

    charging::model::Station station_;
    charging::model::Charger charger_;
    charging::client::services::reservation::ReservationService* service_ = nullptr;
    bool submitting_ = false;

    QComboBox* durationComboBox_ = nullptr;
    QLabel* feeLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    QPushButton* submitButton_ = nullptr;
};

} // namespace charging::client::pages::station
