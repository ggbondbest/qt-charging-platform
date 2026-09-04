#include "settings_service.h"

#include <QCryptographicHash>
#include <QSettings>

#include <algorithm>

namespace charging::client::services::settings {

namespace {

// QSettings 持久化键（组织/应用名在 client/app/main.cpp 统一设置）。
constexpr char kPasswordHashKey[] = "settings/security/passwordHash";
constexpr char kProtectionEnabledKey[] = "settings/security/protectionEnabled";

QString hashPassword(const QString& password)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(password.toUtf8(),
                                 QCryptographicHash::Sha256).toHex());
}

} // namespace

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
    for (const Vehicle& vehicle : vehicles_) {
        nextVehicleId_ = qMax(nextVehicleId_, vehicle.id + 1);
    }
    emit vehiclesChanged();
}

// —— 账号安全（二级保护密码）——

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
    QSettings settings;
    settings.remove(QLatin1String(kPasswordHashKey));
    settings.remove(QLatin1String(kProtectionEnabledKey));
    emit protectionStateChanged();
}

// —— 通知与提醒（QSettings 持久化，默认全开）——

QString SettingsService::notificationKey(Notification key)
{
    switch (key) {
    case Notification::ReservationExpiryReminder:
        return QStringLiteral("settings/notifications/reservationExpiryReminder");
    case Notification::ReservationSuccessNotice:
        return QStringLiteral("settings/notifications/reservationSuccessNotice");
    case Notification::ReservationCancelNotice:
        return QStringLiteral("settings/notifications/reservationCancelNotice");
    }
    return QString();
}

bool SettingsService::notificationEnabled(Notification key) const
{
    QSettings settings;
    return settings.value(notificationKey(key), true).toBool();
}

void SettingsService::setNotificationEnabled(Notification key, bool enabled)
{
    QSettings settings;
    settings.setValue(notificationKey(key), enabled);
    emit notificationsChanged();
}

void SettingsService::resetForTesting()
{
    QSettings settings;
    settings.remove(QLatin1String(kPasswordHashKey));
    settings.remove(QLatin1String(kProtectionEnabledKey));
    settings.remove(notificationKey(Notification::ReservationExpiryReminder));
    settings.remove(notificationKey(Notification::ReservationSuccessNotice));
    settings.remove(notificationKey(Notification::ReservationCancelNotice));
    settings.sync();
    vehicles_.clear();
    nextVehicleId_ = 1;
}

} // namespace charging::client::services::settings
