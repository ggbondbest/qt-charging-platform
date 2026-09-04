#pragma once

#include "services/settings/settings_service.h"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QDialog;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace charging::client::pages::station {

// 设置独立页面（成员 2，任务 #17 二次迭代）：个人中心“⚙️ 设置”入口进入
// 的路由页（HomeShell 索引 7，非底部 Tab；“‹ 返回”由全局 TopNavBar 承担，
// 导航组件复用、不重复开发）。三大模块自上而下：
//
// 1. 账号安全——二级保护密码：设置/修改密码对话框（两次输入一致 + 长度
//    校验，仅存 SHA-256 哈希不落明文）；保护开关未设置密码时置灰不可用，
//    引导先设密码（Service 层同样兜底拒绝）。
// 2. 车辆管理——多台车增删改（车牌/品牌型号/电池容量/接口类型/设为默认），
//    列表卡片复用 ClickableCard 体系；空列表友好提示“预约需先添加车辆”；
//    页内 caption 说明业务联动：**车辆数 = 可预约时段名额**。
// 3. 通知与提醒——预约到期提醒 / 成功通知 / 取消通知三个开关，切换即写
//    QSettings 本地持久化，重进页面回读。
//
// 数据层为 SettingsService（HomeShell 注入，与预约链路共用同一实例：
// 车辆增删改实时联动预约名额与确认页下拉）。长内容置于 QScrollArea，
// 鼠标滚轮上下滚动；样式仅页面局部，不改成员 3 的全局 QSS。
class SettingsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);

    // 非拥有：由 HomeShell 注入；服务状态变化自动联动刷新。
    void setSettingsService(charging::client::services::settings::SettingsService* settings);

    // 依据服务当前状态重建三个区块（路由进入时由壳调用）。
    void refresh();

    // 测试探针。
    int vehicleCardCount() const;
    bool protectionSwitchEnabled() const;
    bool protectionSwitchChecked() const;
    QString passwordStatusText() const;
    QString slotsCaptionText() const;

private:
    QWidget* buildSecuritySection();
    QWidget* buildVehicleSection();
    QWidget* buildNotificationSection();
    void refreshSecuritySection();
    void refreshVehicleSection();
    void refreshNotificationSection();
    QWidget* createVehicleCard(const charging::client::services::settings::Vehicle& vehicle);
    void openPasswordDialog();
    // vehicleId == 0 → 新增；否则编辑既有车辆。
    void openVehicleDialog(qint64 vehicleId);

    charging::client::services::settings::SettingsService* settings_ = nullptr; // not owned

    // 账号安全。
    QLabel* passwordStatusLabel_ = nullptr;
    QPushButton* passwordButton_ = nullptr;
    QCheckBox* protectionSwitch_ = nullptr;
    QLabel* protectionHintLabel_ = nullptr;

    // 车辆管理。
    QWidget* vehiclesHost_ = nullptr;
    QVBoxLayout* vehiclesLayout_ = nullptr;
    QLabel* vehiclesEmptyLabel_ = nullptr;
    QLabel* slotsCaptionLabel_ = nullptr;

    // 通知与提醒。
    QCheckBox* expirySwitch_ = nullptr;
    QCheckBox* successSwitch_ = nullptr;
    QCheckBox* cancelSwitch_ = nullptr;

    QPointer<QDialog> passwordDialog_;
    QPointer<QDialog> vehicleDialog_;
};

} // namespace charging::client::pages::station
