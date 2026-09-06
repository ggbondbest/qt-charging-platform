// NotificationService 单元测试（迭代 3）：三类通知生成、与 SettingsService
// 三开关的联动过滤、notificationsChanged 转发、列表上限与重置口径。
#include "services/favorites/notification_service.h"
#include "services/settings/settings_service.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QtTest>

using charging::client::services::favorites::NotificationItem;
using charging::client::services::favorites::NotificationService;
using charging::client::services::favorites::NotificationType;
using charging::client::services::settings::SettingsService;

class NotificationServiceTest final : public QObject
{
    Q_OBJECT

    NotificationService service_;
    SettingsService settings_;

private slots:
    void initTestCase()
    {
        // 与设置服务同域的隔离配置域（本用例真实写通知开关到 QSettings）。
        QCoreApplication::setOrganizationName(QStringLiteral("ChargingPlatformTeam"));
        QCoreApplication::setApplicationName(QStringLiteral("NotificationServiceTest"));
    }

    void init()
    {
        settings_.resetForTesting(); // 三开关回到默认全开
        service_.resetForTesting();
    }

    void seededHistoryCoversAllTypes()
    {
        const QVector<NotificationItem> items = service_.notifications();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(0).type, NotificationType::ReservationSuccessNotice); // 新在前
        QCOMPARE(items.at(1).type, NotificationType::ReservationExpiryReminder);
        QCOMPARE(items.at(2).type, NotificationType::ReservationCancelNotice);
        QVERIFY(items.at(0).createdAtUtc >= items.at(1).createdAtUtc);
        QVERIFY(items.at(1).createdAtUtc >= items.at(2).createdAtUtc);
    }

    void pushAddsItemAndEmits()
    {
        QSignalSpy spy(&service_, &NotificationService::notificationsChanged);
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(3600);
        service_.pushReservationSuccess(QStringLiteral("测试站"),
                                        QStringLiteral("TST-01-01"),
                                        QStringLiteral("粤B·12345"), start);
        QCOMPARE(spy.count(), 1);
        const QVector<NotificationItem> items = service_.notifications();
        QCOMPARE(items.size(), 4);
        QVERIFY(items.at(0).title.contains(QStringLiteral("✅")));
        QVERIFY(items.at(0).body.contains(QStringLiteral("测试站")));
        QVERIFY(items.at(0).body.contains(QStringLiteral("粤B·12345")));
        QVERIFY(items.at(0).createdAtUtc.isValid());
    }

    void switchLinkageHidesType()
    {
        service_.setSettingsService(&settings_);
        QCOMPARE(service_.visibleCount(), 3); // 默认全开：三类全展示

        settings_.setNotificationEnabled(
            SettingsService::Notification::ReservationSuccessNotice, false);
        QCOMPARE(service_.visibleCount(), 2); // ✅ 类被隐藏（数据仍在）
        const QVector<NotificationItem> items = service_.notifications();
        for (const NotificationItem& item : items) {
            QVERIFY(item.type != NotificationType::ReservationSuccessNotice);
        }

        settings_.setNotificationEnabled(
            SettingsService::Notification::ReservationSuccessNotice, true);
        QCOMPARE(service_.visibleCount(), 3); // 重新打开即恢复展示
    }

    void switchChangeForwardsSignal()
    {
        service_.setSettingsService(&settings_);
        QSignalSpy spy(&service_, &NotificationService::notificationsChanged);
        settings_.setNotificationEnabled(
            SettingsService::Notification::ReservationCancelNotice, false);
        QCOMPARE(spy.count(), 1); // 页面据此即时重渲染
    }

    void expiryReminderDistinguishesLate()
    {
        service_.pushReservationExpired(QStringLiteral("迟到站"),
                                        QStringLiteral("TST-02-01"), /*late=*/true);
        service_.pushReservationExpired(QStringLiteral("到期站"),
                                        QStringLiteral("TST-03-01"), /*late=*/false);
        const QVector<NotificationItem> items = service_.notifications();
        QCOMPARE(items.size(), 5);
        QCOMPARE(items.at(0).type, NotificationType::ReservationExpiryReminder);
        QCOMPARE(items.at(1).type, NotificationType::ReservationExpiryReminder);
        QVERIFY(items.at(0).body.contains(QStringLiteral("时段已结束"))); // 后推在前
        QVERIFY(items.at(1).body.contains(QStringLiteral("自动取消")));   // late 文案
    }

    void listIsCapped()
    {
        for (int i = 0; i < 80; ++i) {
            service_.pushReservationCancelled(QStringLiteral("站%1").arg(i),
                                              QStringLiteral("TST-XX"));
        }
        const QVector<NotificationItem> items = service_.notifications();
        QVERIFY(items.size() <= 50);         // 上限裁剪
        QVERIFY(items.first().body.contains(QStringLiteral("站79"))); // 新在前
    }

    void emptyTypeYieldsEmptyListWithoutCrash()
    {
        // 全关 + 清历史 → 空态（页面展示“暂无通知”的口径来源）。
        service_.setSettingsService(&settings_);
        settings_.setNotificationEnabled(
            SettingsService::Notification::ReservationExpiryReminder, false);
        settings_.setNotificationEnabled(
            SettingsService::Notification::ReservationSuccessNotice, false);
        settings_.setNotificationEnabled(
            SettingsService::Notification::ReservationCancelNotice, false);
        QCOMPARE(service_.visibleCount(), 0);
    }

    void cleanupTestCase()
    {
        settings_.resetForTesting();
    }
};

QTEST_GUILESS_MAIN(NotificationServiceTest)

#include "tst_notification_service.moc"
