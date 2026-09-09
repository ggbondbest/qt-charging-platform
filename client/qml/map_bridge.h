#pragma once

#include "services/map/map_geo_service.h"

#include <QObject>
#include <QVariantList>
#include <QStringList>

namespace charging::qml {

// QML owns no coordinates or route simulation: this bridge exposes Tencent
// results and an explicitly selected address origin (not device GPS).
class MapBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasLocation READ hasLocation NOTIFY locationChanged)
    Q_PROPERTY(double latitude READ latitude NOTIFY locationChanged)
    Q_PROPERTY(double longitude READ longitude NOTIFY locationChanged)
    Q_PROPERTY(QString locationLabel READ locationLabel NOTIFY locationChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString routeHtml READ routeHtml NOTIFY changed)
    Q_PROPERTY(int routeDistanceMeters READ routeDistanceMeters NOTIFY changed)
    Q_PROPERTY(int durationMinutes READ durationMinutes NOTIFY changed)
    Q_PROPERTY(QVariantList steps READ steps NOTIFY changed)
    Q_PROPERTY(QStringList availableCities READ availableCities CONSTANT)
    Q_PROPERTY(QString browsingCity READ browsingCity NOTIFY browsingCityChanged)
public:
    explicit MapBridge(QObject* parent = nullptr);
    explicit MapBridge(charging::client::services::map::MapGeoService* service, QObject* parent);

    bool hasLocation() const { return hasLocation_; }
    double latitude() const { return service_->userLocation().latitude; }
    double longitude() const { return service_->userLocation().longitude; }
    QString locationLabel() const { return locationLabel_; }
    bool busy() const { return geocodeRequest_ != 0 || routeRequest_ != 0; }
    QString error() const { return error_; }
    QString routeHtml() const { return routeHtml_; }
    int routeDistanceMeters() const { return routeDistanceMeters_; }
    int durationMinutes() const { return durationMinutes_; }
    QVariantList steps() const { return steps_; }
    QStringList availableCities() const;
    QString browsingCity() const { return browsingCity_; }
    Q_INVOKABLE bool setBrowsingCity(const QString& city);

    Q_INVOKABLE void geocodeAddress(const QString& address);
    Q_INVOKABLE void setUserLocation(double latitude, double longitude);
    Q_INVOKABLE void requestRoute(double targetLatitude, double targetLongitude,
                                  const QString& mode = QStringLiteral("driving"));
    // Great-circle distance from real station coordinates. UI explicitly labels
    // this as straight-line distance; road distance comes only from route API.
    Q_INVOKABLE int distanceMeters(double latitude, double longitude) const;
    Q_INVOKABLE QString mapHtml(const QVariantList& markers) const;
    Q_INVOKABLE void cancelRoute();
signals:
    void locationChanged();
    void changed();
    void browsingCityChanged();
private:
    QString html(const QVariantList& markers, const QVariantList& points) const;
    charging::client::services::map::MapGeoService* service_;
    bool hasLocation_ = false;
    QString browsingCity_ = QStringLiteral("大连市");
    QString locationLabel_;
    QString requestedAddress_;
    QString error_;
    QString routeHtml_;
    QVariantList steps_;
    int routeDistanceMeters_ = -1;
    int durationMinutes_ = -1;
    quint64 geocodeRequest_ = 0;
    quint64 routeRequest_ = 0;
};

} // namespace charging::qml
