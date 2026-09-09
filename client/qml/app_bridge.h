#pragma once

#include <QObject>
#include <QVariantMap>
#include "charging/common/model/models.h"

namespace charging::client {
class WalletService; class OrderService; class ChargingService;
class StatsService; class CouponService; class PointService; class RatingService;
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
