#include "app_bridge.h"

#include "service_bridges.h"

#include "charging/client/profile_charging/charging_service.h"
#include "charging/client/profile_charging/mock_request_transport.h"
#include "charging/client/profile_charging/order_service.h"
#include "charging/client/profile_charging/wallet_service.h"
#include "charging/common/model/models.h"
#include "services/favorites/favorites_service.h"
#include "services/favorites/notification_service.h"
#include "services/map/map_geo_service.h"
#include "services/reservation/reservation_service.h"
#include "services/settings/settings_service.h"
#include "services/station/station_query_service.h"

namespace charging::qml {

namespace {
// Mirror of MockRequestTransport::seedDemoData() — the demo account.
charging::model::User demoUser()
{
    charging::model::User user;
    user.id = 1;
    user.phone = QStringLiteral("13800138000");
    user.nickname = QStringLiteral("用户8000");
    user.balanceCents = 10000;
    user.status = charging::model::UserStatus::Active;
    return user;
}

QVariantMap toMap(const charging::model::User& user)
{
    return QVariantMap{
        {QStringLiteral("id"), user.id},
        {QStringLiteral("phone"), user.phone},
        {QStringLiteral("nickname"), user.nickname},
        {QStringLiteral("avatarKey"), user.avatarKey},
        {QStringLiteral("balanceCents"), user.balanceCents},
    };
}
} // namespace

QmlApp::QmlApp(QObject* parent)
    : QObject(parent)
{
    const charging::model::User user = demoUser();
    user_ = toMap(user);

    // Exactly the HomeShell mock wiring (home_shell.cpp ctor): transport-backed
    // trio + standalone services, with the cross-service setters in order.
    auto* transport = new charging::client::MockRequestTransport();
    transport->setParent(this);
    transport->setUser(user);
    walletService_ = new charging::client::WalletService(transport, this);
    orderService_ = new charging::client::OrderService(transport, this);
    chargingService_ = new charging::client::ChargingService(transport, this);

    reservationService_ = new charging::client::services::reservation::ReservationService(this);
    settingsService_ = new charging::client::services::settings::SettingsService(this);
    mapGeoService_ = new charging::client::services::map::MapGeoService(this);
    reservationService_->setUserId(user.id);
    reservationService_->setSettingsService(settingsService_);
    favoritesService_ = new charging::client::services::favorites::FavoritesService(this);
    notificationService_ =
        new charging::client::services::favorites::NotificationService(this);
    notificationService_->setSettingsService(settingsService_);
    favoritesService_->setCurrentUser(QString::number(user.id));
    stationQueryService_ =
        new charging::client::services::station::StationQueryService(this);

    // Same-name forwarding bridges become the QML-visible services (CONTRACT §1).
    walletBridge_ = new WalletBridge(walletService_, this);
    orderBridge_ = new OrderBridge(orderService_, this);
    chargingBridge_ = new ChargingBridge(chargingService_, this);
    // Keep currentUser in sync so top-bar balances never lag after profile
    // edit / recharge / payment — the three events that mutate balanceCents.
    connect(walletBridge_, &WalletBridge::profileLoaded, this,
            [this](const QVariantMap& user) {
                for (auto it = user.constBegin(); it != user.constEnd(); ++it)
                    user_.insert(it.key(), it.value());
                emit userChanged();
            });
    connect(walletBridge_, &WalletBridge::rechargeCompleted, this,
            [this](qint64, qint64 balanceAfterCents) {
                user_.insert(QStringLiteral("balanceCents"), balanceAfterCents);
                emit userChanged();
            });
    connect(chargingBridge_, &ChargingBridge::paymentCompleted, this,
            [this](qint64, qint64 balanceAfterCents) {
                user_.insert(QStringLiteral("balanceCents"), balanceAfterCents);
                emit userChanged();
            });
}

QObject* QmlApp::walletService() const { return walletBridge_; }
QObject* QmlApp::orderService() const { return orderBridge_; }
QObject* QmlApp::chargingService() const { return chargingBridge_; }
QObject* QmlApp::reservationService() const { return reservationService_; }
QObject* QmlApp::settingsService() const { return settingsService_; }
QObject* QmlApp::mapGeoService() const { return mapGeoService_; }
QObject* QmlApp::favoritesService() const { return favoritesService_; }
QObject* QmlApp::notificationService() const { return notificationService_; }
QObject* QmlApp::stationQueryService() const { return stationQueryService_; }
QObject* QmlApp::authService() const { return nullptr; }  // TODO(contract): tcp only
QVariantMap QmlApp::currentUser() const { return loggedIn_ ? user_ : QVariantMap{}; }

bool QmlApp::login(const QString& phone)
{
    // Mock channel: any of the seeded phone variants signs into the demo
    // account. TODO(contract): real credential validation lives server-side.
    if (phone.trimmed().isEmpty())
        return false;
    loggedIn_ = true;
    emit userChanged();
    emit loginStateChanged();
    return true;
}

void QmlApp::logout()
{
    loggedIn_ = false;
    emit userChanged();
    emit loginStateChanged();
}

} // namespace charging::qml
