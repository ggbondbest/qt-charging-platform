#pragma once

// QmlApp — the only C++ object the QML tree talks to (client/qml/CONTRACT.md §1).
// Owns the service graph exactly like HomeShell wires it today (mock channel),
// and exposes each service as a CONSTANT QObject* property, injected as context
// properties with the contract names. Signals/properties pass through verbatim.
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

namespace charging::client {
class WalletService;
class OrderService;
class ChargingService;
} // namespace charging::client

namespace charging::client::services {
namespace reservation { class ReservationService; }
namespace settings { class SettingsService; }
namespace map { class MapGeoService; }
namespace favorites { class FavoritesService; class NotificationService; }
namespace station { class StationQueryService; }
} // namespace charging::client::services

namespace charging::qml {

class QmlApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* walletService READ walletService CONSTANT)
    Q_PROPERTY(QObject* orderService READ orderService CONSTANT)
    Q_PROPERTY(QObject* chargingService READ chargingService CONSTANT)
    Q_PROPERTY(QObject* reservationService READ reservationService CONSTANT)
    Q_PROPERTY(QObject* settingsService READ settingsService CONSTANT)
    Q_PROPERTY(QObject* mapGeoService READ mapGeoService CONSTANT)
    Q_PROPERTY(QObject* favoritesService READ favoritesService CONSTANT)
    Q_PROPERTY(QObject* notificationService READ notificationService CONSTANT)
    Q_PROPERTY(QObject* stationQueryService READ stationQueryService CONSTANT)
    // AuthService needs a live ClientConnection; mock channel has none.
    // TODO(contract): wire tcp channel (then also NetworkRequestTransport).
    Q_PROPERTY(QObject* authService READ authService CONSTANT)
    Q_PROPERTY(QVariantMap currentUser READ currentUser NOTIFY loginStateChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loginStateChanged)

public:
    explicit QmlApp(QObject* parent = nullptr);

    QObject* walletService() const;
    QObject* orderService() const;
    QObject* chargingService() const;
    QObject* reservationService() const;
    QObject* settingsService() const;
    QObject* mapGeoService() const;
    QObject* favoritesService() const;
    QObject* notificationService() const;
    QObject* stationQueryService() const;
    QObject* authService() const;  // nullptr on mock — see TODO(contract)
    QVariantMap currentUser() const;
    bool loggedIn() const { return loggedIn_; }

    // Shell routing bridge: QML Shell connects to navigateRequested/back.
    Q_INVOKABLE void navigate(const QString& route) { emit navigateRequested(route); }
    Q_INVOKABLE void back() { emit backRequested(); }
    Q_INVOKABLE void showToast(const QString& text, const QString& tone = QStringLiteral("neutral"))
    {
        emit toastRequested(text, tone);
    }
    // Mock-channel login: seeds the demo account (phone 13800138000).
    // TODO(contract): credential rules on the real backend (member 1/5).
    Q_INVOKABLE bool login(const QString& phone);
    Q_INVOKABLE void logout();

signals:
    void navigateRequested(const QString& route);
    void backRequested();
    void toastRequested(const QString& text, const QString& tone);
    void loginStateChanged();

private:
    charging::client::WalletService* walletService_ = nullptr;
    charging::client::OrderService* orderService_ = nullptr;
    charging::client::ChargingService* chargingService_ = nullptr;
    charging::client::services::reservation::ReservationService* reservationService_ = nullptr;
    charging::client::services::settings::SettingsService* settingsService_ = nullptr;
    charging::client::services::map::MapGeoService* mapGeoService_ = nullptr;
    charging::client::services::favorites::FavoritesService* favoritesService_ = nullptr;
    charging::client::services::favorites::NotificationService* notificationService_ = nullptr;
    charging::client::services::station::StationQueryService* stationQueryService_ = nullptr;
    QVariantMap user_;
    bool loggedIn_ = false;
};

} // namespace charging::qml
