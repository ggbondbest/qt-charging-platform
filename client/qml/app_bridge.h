#pragma once

#include <QObject>
#include <QVariantMap>
#include "charging/common/model/models.h"

namespace charging::client {
class WalletService; class OrderService; class ChargingService;
class StatsService; class CouponService; class PointService; class RatingService;
// 经验等级批：前向声明——QmlApp 仅持裸指针并按 QObject* 透传，无需包含完整头文件。
class ProgressService;
class IRequestTransport;
namespace network { class ClientConnection; }
namespace services {
namespace station { class AuthService; class StationQueryService; }
namespace reservation { class ReservationService; }
namespace settings { class SettingsService; }
namespace map { class MapGeoService; }
namespace favorites { class FavoritesService; class NotificationService; }
}
}

namespace charging::qml {
class WalletBridge; class OrderBridge; class ChargingBridge; class StationQueryBridge;
class ReservationBridge; class SettingsBridge; class FavoritesBridge; class NotificationBridge;
class MapBridge;
class WorkflowBridge;

class StatsBridge; class CouponBridge; class PointBridge; class RatingBridge;

// Each login owns a separate graph; obsolete callbacks cannot mutate a new user.
class QmlApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* walletService READ walletService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* workflowService READ workflowService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* orderService READ orderService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* chargingService READ chargingService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* reservationService READ reservationService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* settingsService READ settingsService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* mapGeoService READ mapGeoService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* mapBridge READ mapBridge CONSTANT)
    Q_PROPERTY(QObject* favoritesService READ favoritesService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* notificationService READ notificationService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* stationQueryService READ stationQueryService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* statsService READ statsService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* couponService READ couponService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* pointsService READ pointsService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* ratingsService READ ratingsService NOTIFY servicesChanged)
    // 经验等级批（2026-09-09 新增功能批）·唯一暴露面：引擎进 QML 只经此属性
    //（原委见下两行）。
    // 经验等级/每日任务引擎（2026-09-09）：session 级 QSettings 持久化，
    // 直接透传（无桥——全 QML 友好签名），QML 侧经 App.progressService 取用。
    Q_PROPERTY(QObject* progressService READ progressService NOTIFY servicesChanged)
    Q_PROPERTY(QObject* authService READ authService CONSTANT)
    Q_PROPERTY(QVariantMap currentUser READ currentUser NOTIFY userChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loginStateChanged)
    Q_PROPERTY(bool mockMode READ mockMode CONSTANT)
    Q_PROPERTY(bool checkingOrders READ checkingOrders NOTIFY checkingOrdersChanged)
public:
    explicit QmlApp(QObject* parent = nullptr);
    QmlApp(const QString& host, quint16 port, bool mockMode, QObject* parent = nullptr);
    ~QmlApp() override;
    QObject* walletService() const;
    QObject* workflowService() const;
    QObject* orderService() const;
    QObject* chargingService() const;
    QObject* reservationService() const;
    QObject* settingsService() const;
    QObject* mapGeoService() const;
    QObject* mapBridge() const;
    QObject* favoritesService() const;
    QObject* notificationService() const;
    QObject* stationQueryService() const;
    QObject* statsService() const;
    QObject* couponService() const;
    QObject* pointsService() const;
    QObject* ratingsService() const;
    // 经验等级批：上述 progressService 属性的 READ 函数声明。
    QObject* progressService() const;
    QObject* authService() const;
    QVariantMap currentUser() const;
    bool loggedIn() const { return loggedIn_; }
    bool mockMode() const { return mockMode_; }
    bool checkingOrders() const { return checkingOrders_; }
    Q_INVOKABLE void navigate(const QString& route, const QVariant& arg = {});
    Q_INVOKABLE void back() { emit backRequested(); }
    Q_INVOKABLE void showToast(const QString& text, const QString& tone = QStringLiteral("neutral"))
    { emit toastRequested(text, tone); }
    Q_INVOKABLE bool login(const QString& phone);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void checkBeforeReservation(const QVariantMap& draft);
    Q_INVOKABLE void recoverUnfinishedOrder();
    // 合并后全绿批（2026-09-08）：预约前置检查的测试缝声明，供 mock 通道清空非
    // 终态订单后从空态测闸（英文说明照抄如下，即本批原注释）。
    // Test seam (setApiKeyForTesting precedent, mock channel only): cancel all
    // non-terminal seeded orders so unfinished-check-gated flows can be
    // exercised from the empty state. No-op on the live transport.
    void clearUnfinishedOrdersForTesting();
    // Native file chooser is compatible with Qt 6.2.4.
    Q_INVOKABLE QString chooseAvatar();
    Q_INVOKABLE QString prepareAvatar(const QString& localFile);
    Q_INVOKABLE QString displayTime(const QString& isoUtc) const;
signals:
    void navigateRequested(const QString& route, const QVariant& arg);
    void backRequested();
    void toastRequested(const QString& text, const QString& tone);
    void loginStateChanged();
    void userChanged();
    void servicesChanged();
    void checkingOrdersChanged();
    void loginStarted();
    void loginSucceeded(const QVariantMap& user, bool created);
    void loginFailed(const QString& message);
private:
    void createSession(const charging::model::User& user);
    void checkUnfinished(const QVariantMap& draft, bool reserveAfter);
    void setCheckingOrders(bool checking);
    charging::client::network::ClientConnection* connection_ = nullptr;
    charging::client::services::station::AuthService* auth_ = nullptr;
    QObject* session_ = nullptr;
    charging::client::IRequestTransport* transport_ = nullptr;
    charging::client::WalletService* walletService_ = nullptr;
    charging::client::OrderService* orderService_ = nullptr;
    charging::client::ChargingService* chargingService_ = nullptr;
    WalletBridge* walletBridge_ = nullptr;
    WorkflowBridge* workflowBridge_ = nullptr;
    OrderBridge* orderBridge_ = nullptr;
    ChargingBridge* chargingBridge_ = nullptr;
    StationQueryBridge* stationQueryBridge_ = nullptr;
    ReservationBridge* reservationBridge_ = nullptr;
    SettingsBridge* settingsBridge_ = nullptr;
    FavoritesBridge* favoritesBridge_ = nullptr;
    NotificationBridge* notificationBridge_ = nullptr;
    MapBridge* mapBridge_ = nullptr;
    charging::client::services::reservation::ReservationService* reservationService_ = nullptr;
    charging::client::services::settings::SettingsService* settingsService_ = nullptr;
    charging::client::services::map::MapGeoService* mapGeoService_ = nullptr;
    charging::client::services::favorites::FavoritesService* favoritesService_ = nullptr;
    charging::client::services::favorites::NotificationService* notificationService_ = nullptr;
    charging::client::services::station::StationQueryService* stationQueryService_ = nullptr;
    charging::client::StatsService* statsService_ = nullptr;
    charging::client::CouponService* couponService_ = nullptr;
    charging::client::PointService* pointService_ = nullptr;
    charging::client::RatingService* ratingService_ = nullptr;
    // 经验等级批：会话级引擎实例指针，createSession 重建、随 session_ 生灭。
    charging::client::ProgressService* progressService_ = nullptr;
    // 2026-09-09 需求批：升级礼包入账的挂起上下文——toast 不再在 levelUp 瞬间
    // 喊"已记入等级账目"（那是分账时代的旧词），而是等 CREDIT_LEVEL_REWARD
    // 服务端回执后如实播报"已到账"；失败自动重试一次，再失败提示下次启动
    // 补送（登录对账重放幂等请求，真到账）。level<=0 即无挂起礼包。
    struct PendingReward {
        int level = 0;
        QString tier;
        qint64 giftPoints = 0;
        bool retried = false;
    };
    PendingReward pendingReward_;
    // 登录对账待补送的礼包档位队列（回执链式取下一笔，见 app_bridge.cpp）。
    QList<int> pendingReconcile_;
    StatsBridge* statsBridge_ = nullptr;
    CouponBridge* couponBridge_ = nullptr;
    PointBridge* pointBridge_ = nullptr;
    RatingBridge* ratingBridge_ = nullptr;
    QVariantMap user_;
    quint64 generation_ = 0;
    bool mockMode_ = false;
    bool loggedIn_ = false;
    bool checkingOrders_ = false;
    bool loggingOut_ = false;
};
}
