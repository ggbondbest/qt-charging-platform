// SettingsService 接口（成员 2，设置域：账号安全/车辆管理/通知开关批）。
// 职责：个人中心“设置”页数据层——二级保护密码（只存哈希）、车辆档案
// （数量即预约名额来源，ReservationService 经注入读取）、通知开关
// （NotificationService 据此门控推送）、外观（成员 3 批次A 追加，见下方
// 接口注释）。
// 使用方：widgets SettingsPage 与 QML AppBridge（SettingsBridge 挂给 QML
// 页面）读写；HomeShell 同时注入给 ReservationService/NotificationService。
// 数据流向：纯本地通道、零网络——状态读写即时落 QSettings（组织/应用名
// 在 client/app/main.cpp 统一设置）；车辆列表为进程内存态（vehicles_），
// QSettings 现势键仅安全/通知/外观三族。
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
// 3) 通知与提醒——预约到期提醒/成功通知/取消通知 + 充电结束/支付成功
//    （2026-09-08 服务端通道追加）共五个开关，
//    QSettings 本地持久化（组织/应用名在 client/app/main.cpp 设置）。
//
// 本服务为纯本地通道（无网络请求）；真实后端 SETTINGS/VEHICLE 命令就绪
// 后可按 ReservationService 同款双通道模式扩展，页面代码不变。
// 读出统一口径：除车辆列表外全部 getter 即时读 QSettings、不做内存缓存，
// 多入口（widgets 页/QML/其他服务）改盘后本服务总能读到最新值。
class SettingsService final : public QObject
{
    Q_OBJECT

public:
    // 通知开关键（与 QSettings 持久化键一一对应）。
    // 2026-09-08 追加服务端通道两值（成员 3 横闯：GET_NOTIFICATIONS 类型接真
    // 数据）——**只可在尾部追加**，与 favorites::NotificationType 的 int 对拍
    // 约定依赖值序（tst_settings_service 对拍用例同步）。
    // 枚举值=存储槽位：每个值经 .cpp notificationKey() 一对一映射到
    // QSettings 键，页面/桥接层只经枚举访问、不感知键字符串。
    enum class Notification
    {
        ReservationExpiryReminder, // 🔔 预约到期提醒
        ReservationSuccessNotice,  // ✅ 预约成功通知
        ReservationCancelNotice,   // ❌ 预约取消通知
        ChargingStopped,           // 🔌 充电结束通知（服务端通道）
        OrderPaid,                 // 💰 支付成功通知（服务端通道）
    };

    explicit SettingsService(QObject* parent = nullptr);

    // —— 车辆管理 ——
    // 只读访问器：引用/裸指针仅在当次读取有效（增删改就地重建列表），
    // 页面不得跨 vehiclesChanged 缓存，信号到达后重新读取。
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
    // 有无密码=存储哈希非空；校验/设置全程只比对摘要，明文永不过磁盘。
    bool hasProtectionPassword() const;
    // 长度兜底与设置页输入校验同口径（≥4），绕过 UI 也拦得住；
    // 失败仅返回 false、不改存储不发信号。
    bool setProtectionPassword(const QString& password); // 校验：长度 ≥ 4
    bool verifyProtectionPassword(const QString& password) const;
    bool protectionEnabled() const;
    // 未设置密码时返回 false 且不改变状态（UI 据此保持开关置灰）。
    bool setProtectionEnabled(bool enabled);
    void clearProtectionPassword();

    // —— 通知与提醒（QSettings 持久化，默认全开）——
    // “默认全开”= 键缺失取 true：首装零键即全开，用户拨动过一次才落盘。
    bool notificationEnabled(Notification key) const;
    void setNotificationEnabled(Notification key, bool enabled);

    // —— 外观（2026-09-08 批次A，成员3 追加）——
    // theme ∈ "light" | "dark"（默认 light）；fontScale ∈ "standard" |
    // "large" | "extraLarge"（默认 standard）。白名单外 set* 返回 false 且
    // 不改状态；get* 恒返回词表内值（被写脏的存储按默认档读回）。
    QString theme() const;
    bool setTheme(const QString& theme);
    QString fontScale() const;
    bool setFontScale(const QString& scale);

    // 清除本服务全部本地持久化（测试隔离用）。
    // ForTesting 命名约定＝测试缝：只被单测调用，生产路径不触碰；
    // 逐键删除而非 clear()，不误伤同一存储里其他服务（收藏等）的键。
    void resetForTesting();

signals:
    // 变更广播一律不带参数：细粒度信息页面经 getter 回读，
    // 避免把整份车辆列表/键值塞进信号跨线程拷贝。
    void vehiclesChanged();
    void protectionStateChanged();
    void notificationsChanged();
    void appearanceChanged();   // 主题/字号任一变更（批次A）

private:
    static QString notificationKey(Notification key);

    QVector<Vehicle> vehicles_;
    qint64 nextVehicleId_ = 1;
};

} // namespace charging::client::services::settings
