// SettingsService 单元测试（任务 #17 二次迭代）：车辆管理 CRUD 与默认车
// 规整、二级保护密码（哈希存储 + 开关前置条件）、通知开关 QSettings 持久化。
#include "services/settings/settings_service.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <QtTest>

using charging::client::services::settings::SettingsService;
using charging::client::services::settings::Vehicle;

namespace {

Vehicle makeVehicle(const QString& plate, bool isDefault = false,
                    charging::model::ChargerType type = charging::model::ChargerType::Fast)
{
    Vehicle vehicle;
    vehicle.plate = plate;
    vehicle.brandModel = QStringLiteral("测试品牌");
    vehicle.batteryKwh = 60;
    vehicle.connectorType = type;
    vehicle.isDefault = isDefault;
    return vehicle;
}

} // namespace

class SettingsServiceTest final : public QObject
{
    Q_OBJECT

    SettingsService service_;

private slots:
    void initTestCase()
    {
        // 独立于生产 app 名的测试配置域，避免污染真实用户设置。
        QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatformTeam"));
        QCoreApplication::setApplicationName(QStringLiteral("SettingsServiceTest"));
    }

    void init()
    {
        service_.resetForTesting();
    }

    // —— 车辆管理 ——

    void addVehicleAssignsIdsAndFirstBecomesDefault()
    {
        QSignalSpy spy(&service_, &SettingsService::vehiclesChanged);
        const qint64 first = service_.addVehicle(makeVehicle(QStringLiteral("粤B·D00001")));
        const qint64 second = service_.addVehicle(makeVehicle(QStringLiteral("粤B·D00002")));
        QVERIFY(first > 0);
        QVERIFY(second > first);
        QCOMPARE(service_.vehicleCount(), 2);
        // 首台自动默认；第二台不抢默认。
        QCOMPARE(service_.defaultVehicle()->id, first);
        QCOMPARE(spy.count(), 2);
    }

    void newDefaultFlagClearsPreviousDefault()
    {
        const qint64 first = service_.addVehicle(makeVehicle(QStringLiteral("粤B·D00001")));
        const qint64 second
            = service_.addVehicle(makeVehicle(QStringLiteral("粤B·D00002"), true));
        QCOMPARE(service_.vehicle(first)->isDefault, false);
        QCOMPARE(service_.defaultVehicle()->id, second);
        // 默认车至多一台。
        int defaults = 0;
        for (const auto& vehicle : service_.vehicles()) {
            defaults += vehicle.isDefault ? 1 : 0;
        }
        QCOMPARE(defaults, 1);
    }

    void updateAndRemoveKeepDefaultInvariant()
    {
        const qint64 first = service_.addVehicle(makeVehicle(QStringLiteral("粤B·D00001")));
        const qint64 second = service_.addVehicle(makeVehicle(QStringLiteral("粤B·D00002")));

        Vehicle edited = *service_.vehicle(second);
        edited.plate = QStringLiteral("粤B·D99999");
        edited.batteryKwh = 80;
        edited.connectorType = charging::model::ChargerType::Slow;
        QVERIFY(service_.updateVehicle(edited));
        QCOMPARE(service_.vehicle(second)->plate, QStringLiteral("粤B·D99999"));
        QCOMPARE(service_.vehicle(second)->batteryKwh, 80);
        QVERIFY(service_.vehicle(second)->connectorType == charging::model::ChargerType::Slow);
        QVERIFY(!service_.updateVehicle(makeVehicle(QStringLiteral("粤B·N0000")))); // 不存在

        service_.setDefaultVehicle(second);
        QCOMPARE(service_.defaultVehicle()->id, second);
        // 取消唯一默认：编辑不勾选默认 → 首台接任（不变式“有车必有默认”）。
        Vehicle uncheck = *service_.vehicle(second);
        uncheck.isDefault = false;
        service_.updateVehicle(uncheck);
        QCOMPARE(service_.defaultVehicle()->id, first);

        // 删除默认车 → 剩余首台自动接任。
        QVERIFY(service_.removeVehicle(first));
        QCOMPARE(service_.defaultVehicle()->id, second);
        QVERIFY(!service_.removeVehicle(first)); // 已不存在
        QVERIFY(service_.removeVehicle(second));
        QCOMPARE(service_.vehicleCount(), 0);
        QVERIFY(service_.defaultVehicle() == nullptr);
    }

    void setMockVehiclesNormalizesDefaults()
    {
        service_.setMockVehicles({makeVehicle(QStringLiteral("粤B·D1"), false),
                                  makeVehicle(QStringLiteral("粤B·D2"), true),
                                  makeVehicle(QStringLiteral("粤B·D3"), true)});
        QCOMPARE(service_.vehicleCount(), 3);
        // 至多一台默认：保留首个带标记的。
        QCOMPARE(service_.defaultVehicle()->plate, QStringLiteral("粤B·D2"));
        // 无标记时首台接任；id 自动续号。
        service_.setMockVehicles({makeVehicle(QStringLiteral("粤B·D4"), false)});
        QCOMPARE(service_.defaultVehicle()->plate, QStringLiteral("粤B·D4"));
        const qint64 added = service_.addVehicle(makeVehicle(QStringLiteral("粤B·D5")));
        QVERIFY(added > service_.vehicles().first().id);
    }

    // —— 账号安全 ——

    void protectionPasswordStoredAsHashOnly()
    {
        QVERIFY(!service_.hasProtectionPassword());
        QVERIFY(!service_.setProtectionPassword(QStringLiteral("123"))); // 长度不足
        QVERIFY(service_.setProtectionPassword(QStringLiteral("abcd1234")));
        QVERIFY(service_.hasProtectionPassword());
        QVERIFY(service_.verifyProtectionPassword(QStringLiteral("abcd1234")));
        QVERIFY(!service_.verifyProtectionPassword(QStringLiteral("wrong")));

        // 明文不落盘：QSettings 中只能找到 64 位十六进制 SHA-256。
        QSettings settings;
        const QString stored
            = settings.value(QStringLiteral("settings/security/passwordHash")).toString();
        QCOMPARE(stored.size(), 64);
        QVERIFY(!stored.contains(QStringLiteral("abcd1234")));
        QCOMPARE(stored, QString::fromLatin1(QCryptographicHash::hash(
                             QStringLiteral("abcd1234").toUtf8(),
                             QCryptographicHash::Sha256).toHex()));

        QSignalSpy spy(&service_, &SettingsService::protectionStateChanged);
        service_.clearProtectionPassword();
        QCOMPARE(spy.count(), 1);
        QVERIFY(!service_.hasProtectionPassword());
    }

    void protectionSwitchRequiresPassword()
    {
        // 未设置密码：开关不可开启（Service 兜底，UI 置灰依据）。
        QVERIFY(!service_.setProtectionEnabled(true));
        QVERIFY(!service_.protectionEnabled());

        QVERIFY(service_.setProtectionPassword(QStringLiteral("pin-2580")));
        QVERIFY(service_.setProtectionEnabled(true));
        QVERIFY(service_.protectionEnabled());

        // 持久化：重建实例（同一 QSettings 域）读取开关状态。
        SettingsService reopened;
        QVERIFY(reopened.hasProtectionPassword());
        QVERIFY(reopened.protectionEnabled());
        QVERIFY(reopened.verifyProtectionPassword(QStringLiteral("pin-2580")));
    }

    // —— 通知与提醒 ——

    void notificationTogglesPersistAcrossInstances()
    {
        // 默认全开。
        QVERIFY(service_.notificationEnabled(
            SettingsService::Notification::ReservationExpiryReminder));
        QVERIFY(service_.notificationEnabled(
            SettingsService::Notification::ReservationSuccessNotice));
        QVERIFY(service_.notificationEnabled(
            SettingsService::Notification::ReservationCancelNotice));

        QSignalSpy spy(&service_, &SettingsService::notificationsChanged);
        service_.setNotificationEnabled(SettingsService::Notification::ReservationSuccessNotice,
                                        false);
        service_.setNotificationEnabled(SettingsService::Notification::ReservationCancelNotice,
                                        false);
        QCOMPARE(spy.count(), 2);

        // 重进页面（新实例）回读：仅到期提醒保持开启。
        SettingsService reopened;
        QVERIFY(reopened.notificationEnabled(
            SettingsService::Notification::ReservationExpiryReminder));
        QVERIFY(!reopened.notificationEnabled(
            SettingsService::Notification::ReservationSuccessNotice));
        QVERIFY(!reopened.notificationEnabled(
            SettingsService::Notification::ReservationCancelNotice));

        reopened.resetForTesting();
        QVERIFY(reopened.notificationEnabled(
            SettingsService::Notification::ReservationSuccessNotice)); // 复位默认全开
    }
};

QTEST_GUILESS_MAIN(SettingsServiceTest)

#include "tst_settings_service.moc"
