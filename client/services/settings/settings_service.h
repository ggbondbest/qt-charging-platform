#pragma once

#include "charging/common/model/enums.h"

#include <QObject>
#include <QVector>

namespace charging::client::services::settings {

// 用户车辆档案（任务 #17 二次迭代）：设置页-车辆管理的数据模型。
// 后端 VEHICLE 命令与数据表尚未定义（protocol/common 属成员 1/3 领域），
// 当前为客户端 Service 模拟 + 本地持久化；接口就绪后信号形状不变，
// UI 零改动切换真实通道。
struct Vehicle
{
    qint64 id = 0;
    QString plate;                                    // 车牌号码
    QString brandModel;                               // 品牌型号
    int batteryKwh = 0;                               // 电池容量（kWh）
    charging::model::ChargerType connectorType
        = charging::model::ChargerType::Fast;         // 接口类型（快充/慢充）
    bool isDefault = false;                           // 默认车辆（预约默认选用）
};

// 设置服务（成员 2）：个人中心“⚙️ 设置”独立页面的数据层，三大模块：
// 1) 账号安全——二级保护密码（仅存 SHA-256 哈希，不落明文）与开关
//    （未设置密码时开关不可用，引导先设密码）；
// 2) 车辆管理——多台车增删改、默认车辆（至多一台）；车辆数量决定
//    用户可同时持有的有效预约名额（ReservationService 读取）；默认车
//    接口类型用于站点详情/预约场景的充电桩匹配提示；
// 3) 通知与提醒——预约到期提醒/成功通知/取消通知三个开关，
//    QSettings 本地持久化（组织/应用名在 client/app/main.cpp 设置）。
//
// 本服务为纯本地通道（无网络请求）；真实后端 SETTINGS/VEHICLE 命令就绪
// 后可按 ReservationService 同款双通道模式扩展，页面代码不变。
class SettingsService final : public QObject
{
    Q_OBJECT

public:
    // 通知开关键（与 QSettings 持久化键一一对应）。
    enum class Notification
    {
        ReservationExpiryReminder, // 🔔 预约到期提醒
        ReservationSuccessNotice,  // ✅ 预约成功通知
        ReservationCancelNotice,   // ❌ 预约取消通知
    };

    explicit SettingsService(QObject* parent = nullptr);

    // —— 车辆管理 ——
    const QVector<Vehicle>& vehicles() const;
    int vehicleCount() const;
    const Vehicle* vehicle(qint64 id) const;
    const Vehicle* defaultVehicle() const; // 无车时为 nullptr

    // 新增车辆（draft.id 被忽略，自动分配）；首台车自动成为默认车；
    // draft.isDefault 为 true 时清除其余车辆的默认标记。返回新车 ID。
    qint64 addVehicle(const Vehicle& draft);
    bool updateVehicle(const Vehicle& vehicle);
    bool removeVehicle(qint64 id); // 删除默认车后自动把剩余首台设为默认
    void setDefaultVehicle(qint64 id);
    // 演示/测试：整体覆盖车辆列表（规整默认标记：至多一台，空列表无默认）。
    void setMockVehicles(const QVector<Vehicle>& vehicles);

    // —— 账号安全（二级保护密码）——
    bool hasProtectionPassword() const;
    bool setProtectionPassword(const QString& password); // 校验：长度 ≥ 4
    bool verifyProtectionPassword(const QString& password) const;
    bool protectionEnabled() const;
    // 未设置密码时返回 false 且不改变状态（UI 据此保持开关置灰）。
    bool setProtectionEnabled(bool enabled);
    void clearProtectionPassword();

    // —— 通知与提醒（QSettings 持久化，默认全开）——
    bool notificationEnabled(Notification key) const;
    void setNotificationEnabled(Notification key, bool enabled);

    // 清除本服务全部本地持久化（测试隔离用）。
    void resetForTesting();

signals:
    void vehiclesChanged();
    void protectionStateChanged();
    void notificationsChanged();

private:
    static QString notificationKey(Notification key);

    QVector<Vehicle> vehicles_;
    qint64 nextVehicleId_ = 1;
};

} // namespace charging::client::services::settings
