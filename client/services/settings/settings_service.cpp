// SettingsService 实现（类职责、三大模块与批史见同名头文件）。
// 数据流向：设置页（widgets SettingsPage / QML SettingsBridge）→ 本服务 →
// QSettings 本地存储——安全（密码哈希+保护开关）、通知五开关、外观
// （主题/字号白名单）三族键；车辆管理现势仅存内存 vehicles_（未落盘），
// ReservationService 的名额经注入直接读 vehicleCount()。
#include "settings_service.h"

#include <QCryptographicHash>
#include <QSettings>

#include <algorithm>

namespace charging::client::services::settings {

// ---- 匿名命名空间：QSettings 键常量与密码摘要工具（文件私有）----

namespace {

// QSettings 持久化键（组织/应用名在 client/app/main.cpp 统一设置）。
constexpr char kPasswordHashKey[] = "settings/security/passwordHash";
constexpr char kProtectionEnabledKey[] = "settings/security/protectionEnabled";
// 外观（2026-09-08 批次A，成员3 追加）：主题与字号，白名单值，非法 set 忽略。
constexpr char kThemeKey[] = "settings/appearance/theme";
constexpr char kPaletteKey[] = "settings/appearance/palette";
constexpr char kFontScaleKey[] = "settings/appearance/fontScale";

// 纯 SHA-256 摘要（无盐）：满足课程演示“明文不落盘、校验=重算比对”即可，
// 不构成生产级口令防护，哈希只是本地隐私底线而非安全边界。
QString hashPassword(const QString& password)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(password.toUtf8(),
                                 QCryptographicHash::Sha256).toHex());
}

} // namespace

// 构造刻意留空：安全/通知/外观不入内存缓存，getter 每次现读盘上最新态
// （多入口读写互见）；本服务唯一内存态是车辆列表，由调用方装配。
SettingsService::SettingsService(QObject* parent)
    : QObject(parent)
{
}

// —— 车辆管理 ——

const QVector<Vehicle>& SettingsService::vehicles() const
{
    return vehicles_;
}

int SettingsService::vehicleCount() const
{
    return vehicles_.size();
}

const Vehicle* SettingsService::vehicle(qint64 id) const
{
    for (const Vehicle& vehicle : vehicles_) {
        if (vehicle.id == id) {
            return &vehicle;
        }
    }
    return nullptr;
}

const Vehicle* SettingsService::defaultVehicle() const
{
    for (const Vehicle& vehicle : vehicles_) {
        if (vehicle.isDefault) {
            return &vehicle;
        }
    }
    return nullptr;
}

qint64 SettingsService::addVehicle(const Vehicle& draft)
{
    Vehicle vehicle = draft;
    // 编号服务内分配、只增不回收：删车后号不复用，页面若缓存旧 ID 不会撞车。
    vehicle.id = nextVehicleId_++;
    // 首台车自动成为默认车；指定默认时清除其余车辆的默认标记（至多一台）。
    if (vehicles_.isEmpty()) {
        vehicle.isDefault = true;
    }
    if (vehicle.isDefault) {
        for (Vehicle& other : vehicles_) {
            other.isDefault = false;
        }
    }
    vehicles_.push_back(vehicle);
    emit vehiclesChanged();
    return vehicle.id;
}

bool SettingsService::updateVehicle(const Vehicle& updated)
{
    const int index = static_cast<int>(
        std::find_if(vehicles_.cbegin(), vehicles_.cend(),
                     [id = updated.id](const Vehicle& v) { return v.id == id; })
        - vehicles_.cbegin());
    if (index >= vehicles_.size()) {
        return false;
    }
    // 读-改-写：只覆盖可编辑五字段，id 按主键语义不动。
    Vehicle current = vehicles_[index];
    current.plate = updated.plate;
    current.brandModel = updated.brandModel;
    current.batteryKwh = updated.batteryKwh;
    current.connectorType = updated.connectorType;
    current.isDefault = updated.isDefault;
    if (current.isDefault) {
        for (Vehicle& other : vehicles_) {
            other.isDefault = false;
        }
    }
    vehicles_[index] = current;
    // “有车必有默认”不变式：若本次编辑取消了唯一的默认标记，首台接任。
    if (defaultVehicle() == nullptr) {
        vehicles_.first().isDefault = true;
    }
    emit vehiclesChanged();
    return true;
}

bool SettingsService::removeVehicle(qint64 id)
{
    const int before = vehicles_.size();
    // 删除前先读“是否默认车”：删掉后此信息即失，无法决定要不要让剩余首台接任。
    const bool wasDefault = vehicle(id) != nullptr && vehicle(id)->isDefault;
    vehicles_.erase(
        std::remove_if(vehicles_.begin(), vehicles_.end(),
                       [id](const Vehicle& v) { return v.id == id; }),
        vehicles_.end());
    if (vehicles_.size() == before) {
        return false;
    }
    // 删除的是默认车：剩余首台自动接任，保证“有车必有默认”。
    if (wasDefault && !vehicles_.isEmpty()) {
        vehicles_.first().isDefault = true;
    }
    emit vehiclesChanged();
    return true;
}

void SettingsService::setDefaultVehicle(qint64 id)
{
    // changed 闸：重复点同一默认车时整圈比对无差异、不发信号（幂等，
    // 防页面刷新风暴）；设置本身天然互斥（want 同步翻转其余车辆）。
    bool changed = false;
    for (Vehicle& vehicle : vehicles_) {
        const bool want = (vehicle.id == id);
        if (vehicle.isDefault != want) {
            vehicle.isDefault = want;
            changed = true;
        }
    }
    if (changed) {
        emit vehiclesChanged();
    }
}

void SettingsService::setMockVehicles(const QVector<Vehicle>& vehicles)
{
    vehicles_ = vehicles;
    // 规整默认标记：至多一台为默认；非空但无默认时首台接任。
    bool seenDefault = false;
    for (Vehicle& vehicle : vehicles_) {
        if (!seenDefault && vehicle.isDefault) {
            seenDefault = true;
            continue;
        }
        vehicle.isDefault = false;
    }
    if (!vehicles_.isEmpty() && !seenDefault) {
        vehicles_.first().isDefault = true;
    }
    // 自增计数器推到 max(mock ID)+1：之后 addVehicle 的新车不会与注入列表撞号。
    for (const Vehicle& vehicle : vehicles_) {
        nextVehicleId_ = qMax(nextVehicleId_, vehicle.id + 1);
    }
    emit vehiclesChanged();
}

// —— 账号安全（二级保护密码）——

// “已设密码”＝哈希键非空；每次现构 QSettings 现读盘（无缓存，多入口互见）。
bool SettingsService::hasProtectionPassword() const
{
    QSettings settings;
    return !settings.value(QLatin1String(kPasswordHashKey)).toString().isEmpty();
}

bool SettingsService::setProtectionPassword(const QString& password)
{
    if (password.size() < 4) {
        return false; // 与设置页输入校验一致的 Service 层兜底
    }
    QSettings settings;
    settings.setValue(QLatin1String(kPasswordHashKey), hashPassword(password));
    emit protectionStateChanged();
    return true;
}

bool SettingsService::verifyProtectionPassword(const QString& password) const
{
    QSettings settings;
    const QString stored
        = settings.value(QLatin1String(kPasswordHashKey)).toString();
    return !stored.isEmpty() && stored == hashPassword(password);
}

bool SettingsService::protectionEnabled() const
{
    QSettings settings;
    // 双闸合成：“开关键为真”且“密码仍在”才算开启——哈希被清（本服务
    // 成对删除或外部写脏）后，残留开关键不得虚报开启。
    return hasProtectionPassword()
        && settings.value(QLatin1String(kProtectionEnabledKey), false).toBool();
}

bool SettingsService::setProtectionEnabled(bool enabled)
{
    if (enabled && !hasProtectionPassword()) {
        return false; // 未设置密码不允许开启（UI 开关置灰的兜底）
    }
    QSettings settings;
    settings.setValue(QLatin1String(kProtectionEnabledKey), enabled);
    emit protectionStateChanged();
    return true;
}

void SettingsService::clearProtectionPassword()
{
    // 哈希与开关键成对删除：只清哈希会留下悬空开关键（读侧靠双闸兜住，
    // 但盘面应同步干净），随后广播让 UI 把开关拨回置灰态。
    QSettings settings;
    settings.remove(QLatin1String(kPasswordHashKey));
    settings.remove(QLatin1String(kProtectionEnabledKey));
    emit protectionStateChanged();
}

// —— 通知与提醒（QSettings 持久化，默认全开）——

// 枚举→QSettings 键一一对应；switch 故意不写 default：新增枚举值时编译器
// 告警“未处理分支”，提醒映射与枚举同步（防新开关静默漏存）。
QString SettingsService::notificationKey(Notification key)
{
    switch (key) {
    case Notification::ReservationExpiryReminder:
        return QStringLiteral("settings/notifications/reservationExpiryReminder");
    case Notification::ReservationSuccessNotice:
        return QStringLiteral("settings/notifications/reservationSuccessNotice");
    case Notification::ReservationCancelNotice:
        return QStringLiteral("settings/notifications/reservationCancelNotice");
    case Notification::ChargingStopped:
        return QStringLiteral("settings/notifications/chargingStopped");
    case Notification::OrderPaid:
        return QStringLiteral("settings/notifications/orderPaid");
    }
    // 防御兜底：外部强转出词表外枚举值时映射为空键，不命中任何有效配置。
    return QString();
}

bool SettingsService::notificationEnabled(Notification key) const
{
    QSettings settings;
    // 缺省 true：首装零键即全开（产品口径“默认全开”），拨过一次才落盘。
    return settings.value(notificationKey(key), true).toBool();
}

void SettingsService::setNotificationEnabled(Notification key, bool enabled)
{
    // 直接落盘并广播 notificationsChanged：NotificationService 监听该信号
    // 重做推送门控，页面开关与后台门控同源同刻生效。
    QSettings settings;
    settings.setValue(notificationKey(key), enabled);
    emit notificationsChanged();
}

// —— 外观（主题/字号，2026-09-08 批次A，成员3 追加）——
// 值白名单在 getter 再收一次口：QSettings 里被外部写脏的值按默认档读回，
// UI 永远不会拿到词表外的字符串（Style.qml 的三元绑定据此安全）。

QString SettingsService::theme() const
{
    const QString value
        = QSettings().value(QLatin1String(kThemeKey), QStringLiteral("light")).toString();
    return value == QLatin1String("dark") ? value : QStringLiteral("light");
}

bool SettingsService::setTheme(const QString& theme)
{
    if (theme != QLatin1String("light") && theme != QLatin1String("dark")) {
        return false; // 白名单外：不改状态不发信号
    }
    QSettings settings;
    settings.setValue(QLatin1String(kThemeKey), theme);
    emit appearanceChanged();
    return true;
}

QString SettingsService::fontScale() const
{
    const QString value
        = QSettings().value(QLatin1String(kFontScaleKey), QStringLiteral("standard")).toString();
    return (value == QLatin1String("large") || value == QLatin1String("extraLarge"))
        ? value : QStringLiteral("standard");
}

bool SettingsService::setFontScale(const QString& scale)
{
    if (scale != QLatin1String("standard") && scale != QLatin1String("large")
        && scale != QLatin1String("extraLarge")) {
        return false;
    }
    QSettings settings;
    settings.setValue(QLatin1String(kFontScaleKey), scale);
    emit appearanceChanged();
    return true;
}

QString SettingsService::palette() const
{
    const QString value
        = QSettings().value(QLatin1String(kPaletteKey), QStringLiteral("green")).toString();
    static const QStringList kPaletteWhitelist = {
        QStringLiteral("green"), QStringLiteral("blue"),
        QStringLiteral("violet"), QStringLiteral("amber") };
    return kPaletteWhitelist.contains(value) ? value : QStringLiteral("green");
}

bool SettingsService::setPalette(const QString& palette)
{
    static const QStringList kPaletteWhitelist = {
        QStringLiteral("green"), QStringLiteral("blue"),
        QStringLiteral("violet"), QStringLiteral("amber") };
    if (!kPaletteWhitelist.contains(palette)) {
        return false; // 白名单外：不改状态不发信号
    }
    QSettings settings;
    settings.setValue(QLatin1String(kPaletteKey), palette);
    emit appearanceChanged();
    return true;
}

// 测试缝（ForTesting 命名约定）：仅供 tst_settings_service 用例间隔离调用，
// 生产路径不触碰。
void SettingsService::resetForTesting()
{
    // 逐键枚举删除、不用 clear()：同一配置文件还装着收藏等他人键，全清即误伤。
    QSettings settings;
    settings.remove(QLatin1String(kPasswordHashKey));
    settings.remove(QLatin1String(kProtectionEnabledKey));
    settings.remove(notificationKey(Notification::ReservationExpiryReminder));
    settings.remove(notificationKey(Notification::ReservationSuccessNotice));
    settings.remove(notificationKey(Notification::ReservationCancelNotice));
    settings.remove(notificationKey(Notification::ChargingStopped));
    settings.remove(notificationKey(Notification::OrderPaid));
    settings.remove(QLatin1String(kThemeKey));
    settings.remove(QLatin1String(kFontScaleKey));
    settings.remove(QLatin1String(kPaletteKey));
    // sync()：QSettings 默认延迟落盘，强制即刻冲刷，保证下一个用例新建
    // 实例时看到“已清空”的盘面。
    settings.sync();
    // 内存车辆表与编号计数器同批复位，测试间不残留车辆。
    vehicles_.clear();
    nextVehicleId_ = 1;
}

} // namespace charging::client::services::settings
