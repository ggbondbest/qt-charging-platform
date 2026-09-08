// NotificationService 单元测试（迭代 3 + 2026-09-08 服务端通道扩展）：三类
// 通知生成、与 SettingsService 开关的联动过滤、notificationsChanged 转发、
// 列表上限与重置口径；扩展：GET_NOTIFICATIONS 拉取合并、未知词丢弃、
// 失败静默、服务端段开关联动。
#include "services/favorites/notification_service.h"
#include "services/settings/settings_service.h"

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/protocol/protocol.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

using charging::client::services::favorites::NotificationItem;
using charging::client::services::favorites::NotificationService;
using charging::client::services::favorites::NotificationType;
using charging::client::services::settings::SettingsService;

namespace {

// 同步回执的桩 transport：记录请求并回放预置载荷（mock transport 同款语义，
// 无需事件循环）。
class StubTransport final : public charging::client::IRequestTransport
{
public:
    void send(const QString& type, const QJsonObject& data,
              const ResponseCallback& callback) override
    {
        ++calls;
        lastType = type;
        lastData = data;
        callback(shouldSucceed, payload, charging::protocol::ProtocolError{});
    }

    int calls = 0;
    bool shouldSucceed = true;
    QString lastType;
    QJsonObject lastData;
    QJsonObject payload;
};

} // namespace

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
        settings_.resetForTesting(); // 全部开关回到默认全开
        // 桩 transport 是用例内栈对象：用例间必须摘钩，防成员 service_
        // 持有上一用例已析构的悬垂指针。
        service_.setTransport(nullptr);
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

    // —— 服务端通道（2026-09-08 扩展）——

    void serverTypesHaveTitles()
    {
        QVERIFY(NotificationService::typeTitle(NotificationType::ChargingStopped)
                    .contains(QString::fromUtf8("🔌")));
        QVERIFY(NotificationService::typeTitle(NotificationType::OrderPaid)
                    .contains(QString::fromUtf8("💰")));
    }

    void refreshMergesServerRows()
    {
        StubTransport transport;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        transport.payload = QJsonObject{
            {QStringLiteral("notifications"), QJsonArray{
                QJsonObject{{QStringLiteral("id"), QStringLiteral("2")},
                            {QStringLiteral("type"), QStringLiteral("order_paid")},
                            {QStringLiteral("title"), QStringLiteral("支付成功")},
                            {QStringLiteral("body"), QStringLiteral("订单 77 已支付 ¥12.00")},
                            {QStringLiteral("createdAtUtc"),
                             now.addSecs(-60).toString(Qt::ISODateWithMs)}},
                QJsonObject{{QStringLiteral("id"), QStringLiteral("1")},
                            {QStringLiteral("type"), QStringLiteral("charging_stopped")},
                            {QStringLiteral("title"), QStringLiteral("充电已结束")},
                            {QStringLiteral("body"), QStringLiteral("本次充电 12.34 kWh")},
                            {QStringLiteral("createdAtUtc"),
                             now.addSecs(-120).toString(Qt::ISODateWithMs)}},
                // 无 id 行 + 毫秒数字时间（mock 通道形态）→ 负段合成 id。
                QJsonObject{{QStringLiteral("type"), QStringLiteral("charging_stopped")},
                            {QStringLiteral("title"), QStringLiteral("充电已结束")},
                            {QStringLiteral("body"), QStringLiteral("老单补发")},
                            {QStringLiteral("createdAtUtc"),
                             static_cast<double>(now.addSecs(-3000).toMSecsSinceEpoch())}},
                // 未知类型词：防御性丢弃，不污染列表。
                QJsonObject{{QStringLiteral("type"), QStringLiteral("coupon_granted")},
                            {QStringLiteral("createdAtUtc"),
                             now.addSecs(-30).toString(Qt::ISODateWithMs)}},
            }},
        };
        service_.setTransport(&transport);
        QSignalSpy spy(&service_, &NotificationService::notificationsChanged);
        service_.refresh();

        QCOMPARE(transport.calls, 1);
        QCOMPARE(transport.lastType,
                 QString::fromLatin1(charging::protocol::request_type::kGetNotifications));
        QCOMPARE(transport.lastData.value(QStringLiteral("pageSize")).toInt(), 50);
        QCOMPARE(spy.count(), 1);

        // 期望序（时间倒序合并本地 seed：14/95/1520 分钟前）：
        // -60s 支付、-120s 结束、-840s(14min) seed 成功、-3000s(50min) 补发、
        // -5700s(95min) seed 到期、-91200s seed 取消。
        const QVector<NotificationItem> items = service_.notifications();
        QCOMPARE(items.size(), 6);
        QCOMPARE(items.at(0).type, NotificationType::OrderPaid);
        QCOMPARE(items.at(0).id, qint64(2));
        QCOMPARE(items.at(1).type, NotificationType::ChargingStopped);
        QCOMPARE(items.at(1).id, qint64(1));
        QCOMPARE(items.at(2).type, NotificationType::ReservationSuccessNotice);
        QCOMPARE(items.at(3).type, NotificationType::ChargingStopped);
        QCOMPARE(items.at(3).id, qint64(-1)); // 负段合成，不与本地正整数 id 相撞
        QCOMPARE(items.at(4).type, NotificationType::ReservationExpiryReminder);
        QCOMPARE(items.at(5).type, NotificationType::ReservationCancelNotice);

        // 二次 refresh：服务端段整体替换（空载荷 → 只剩本地段）。
        transport.payload = QJsonObject{};
        service_.refresh();
        QCOMPARE(transport.calls, 2);
        QCOMPARE(service_.notifications().size(), 3);
    }

    void refreshFailureAndTransportlessAreSilent()
    {
        // 未注入 transport：refresh 完全 no-op（迭代 3 行为保持）。
        service_.refresh();
        QCOMPARE(service_.notifications().size(), 3);

        StubTransport transport;
        transport.shouldSucceed = false;
        service_.setTransport(&transport);
        QSignalSpy spy(&service_, &NotificationService::notificationsChanged);
        service_.refresh();
        QCOMPARE(transport.calls, 1);
        QCOMPARE(spy.count(), 0); // 失败不发空信号打扰页面
        QCOMPARE(service_.isRefreshing(), false);
        QCOMPARE(service_.notifications().size(), 3); // 本地段不受服务端失败影响
    }

    void serverRowsRespectSwitches()
    {
        service_.setSettingsService(&settings_);
        StubTransport transport;
        transport.payload = QJsonObject{
            {QStringLiteral("notifications"), QJsonArray{
                QJsonObject{{QStringLiteral("id"), QStringLiteral("9")},
                            {QStringLiteral("type"), QStringLiteral("charging_stopped")},
                            {QStringLiteral("title"), QStringLiteral("充电已结束")},
                            {QStringLiteral("body"), QStringLiteral("合并口径校验")},
                            {QStringLiteral("createdAtUtc"),
                             QDateTime::currentDateTimeUtc().addSecs(-60)
                                 .toString(Qt::ISODateWithMs)}},
            }},
        };
        service_.setTransport(&transport);
        service_.refresh();
        QCOMPARE(service_.visibleCount(), 4); // 默认全开：seed3 + 服务端 1

        settings_.setNotificationEnabled(
            SettingsService::Notification::ChargingStopped, false);
        QCOMPARE(service_.visibleCount(), 3); // 新开关同样参与联动过滤（数据仍在）
        for (const NotificationItem& item : service_.notifications()) {
            QVERIFY(item.type != NotificationType::ChargingStopped);
        }
    }

    void cleanupTestCase()
    {
        settings_.resetForTesting();
    }
};

QTEST_GUILESS_MAIN(NotificationServiceTest)

#include "tst_notification_service.moc"
