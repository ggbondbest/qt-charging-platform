#include "map_bridge.h"

#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>
#include <QtMath>
#include <cmath>

using namespace charging::client::services::map;

namespace charging::qml {
namespace {
bool valid(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
}
}

MapBridge::MapBridge(QObject* parent) : MapBridge(new MapGeoService, parent)
{
    service_->setParent(this);
}

MapBridge::MapBridge(MapGeoService* service, QObject* parent) : QObject(parent), service_(service)
{
    connect(service_, &MapGeoService::forwardGeocodeSucceeded, this,
            [this](quint64 id, LatLng point, const QString&) {
        if (id != geocodeRequest_) return;
        if (!valid(point.latitude, point.longitude)) return;
        geocodeRequest_ = 0;
        cancelRoute();
        service_->setUserLocation(point);
        hasLocation_ = true;
        error_.clear();
        locationLabel_ = requestedAddress_;
        emit locationChanged();
        emit changed();
    });
    connect(service_, &MapGeoService::forwardGeocodeFailed, this,
            [this](quint64 id, MapError, const QString& message) {
        if (id != geocodeRequest_) return;
        geocodeRequest_ = 0;
        error_ = message;
        emit changed();
    });
    connect(service_, &MapGeoService::routeSucceeded, this,
            [this](quint64 id, const RouteResult& route) {
        if (id != routeRequest_) return;
        routeRequest_ = 0;
        if (route.distanceMeters < 0 || route.durationMinutes < 0 || route.polyline.size() < 2) {
            error_ = QStringLiteral("腾讯路线数据不完整，请重试；未生成模拟路线");
            emit changed();
            return;
        }
        QVariantList points;
        for (const auto& point : route.polyline)
            points.append(QVariant(QVariantList{point.latitude, point.longitude}));
        for (const auto& step : route.steps)
            steps_.append(QVariantMap{{QStringLiteral("instruction"), step.instruction},
                                      {QStringLiteral("distanceMeters"), step.distanceMeters}});
        routeDistanceMeters_ = route.distanceMeters;
        durationMinutes_ = route.durationMinutes;
        routeHtml_ = html({}, points);
        emit changed();
    });
    connect(service_, &MapGeoService::routeFailed, this,
            [this](quint64 id, MapError, const QString& message) {
        if (id != routeRequest_) return;
        routeRequest_ = 0;
        error_ = message;
        emit changed();
    });
}

void MapBridge::geocodeAddress(const QString& address)
{
    requestedAddress_ = address.trimmed();
    error_.clear();
    cancelRoute();
    geocodeRequest_ = service_->requestForwardGeocode(requestedAddress_);
    emit changed();
}

void MapBridge::setUserLocation(double latitude, double longitude)
{
    if (!valid(latitude, longitude)) {
        error_ = QStringLiteral("请输入有效经纬度");
        emit changed();
        return;
    }
    geocodeRequest_ = 0;
    cancelRoute();
    service_->setUserLocation({latitude, longitude});
    hasLocation_ = true;
    error_.clear();
    locationLabel_ = QStringLiteral("手动定位 %1, %2").arg(latitude, 0, 'f', 6).arg(longitude, 0, 'f', 6);
    emit locationChanged();
    emit changed();
}

void MapBridge::cancelRoute()
{
    routeRequest_ = 0; // An older HTTP response is ignored, including after page destruction.
    routeHtml_.clear();
    routeDistanceMeters_ = -1;
    durationMinutes_ = -1;
    steps_.clear();
    emit changed();
}

void MapBridge::requestRoute(double targetLatitude, double targetLongitude, const QString& mode)
{
    cancelRoute();
    error_.clear();
    if (!hasLocation_ || geocodeRequest_ != 0)
        error_ = QStringLiteral("请先在附近电站页面完成起点地址定位");
    else if (!valid(targetLatitude, targetLongitude))
        error_ = QStringLiteral("电站坐标无效，无法规划路线");
    else if (mode != QStringLiteral("driving") && mode != QStringLiteral("walking"))
        error_ = QStringLiteral("仅支持驾车或步行");
    else {
        const LatLng to{targetLatitude, targetLongitude};
        routeRequest_ = mode == QStringLiteral("walking")
            ? service_->requestWalkingRoute(service_->userLocation(), to)
            : service_->requestDrivingRoute(service_->userLocation(), to);
    }
    emit changed();
}

int MapBridge::distanceMeters(double latitude, double longitude) const
{
    if (!hasLocation_ || !valid(latitude, longitude)) return -1;
    const double lat1 = qDegreesToRadians(this->latitude());
    const double lat2 = qDegreesToRadians(latitude);
    const double dlat = lat2 - lat1;
    const double dlon = qDegreesToRadians(longitude - this->longitude());
    const double a = std::pow(std::sin(dlat / 2), 2)
        + std::cos(lat1) * std::cos(lat2) * std::pow(std::sin(dlon / 2), 2);
    return qRound(6371008.8 * 2 * std::atan2(std::sqrt(qBound(0.0, a, 1.0)),
                                           std::sqrt(qBound(0.0, 1 - a, 1.0))));
}

QString MapBridge::mapHtml(const QVariantList& markers) const
{
    // Browsing Dalian is not a GPS fix. Distances/routes still require an
    // explicitly geocoded origin; no location request runs on page creation.
    return html(markers, {});
}

QString MapBridge::html(const QVariantList& markers, const QVariantList& points) const
{
    QString key = qEnvironmentVariable("TENCENT_MAP_JS_KEY").trimmed();
    if (key.isEmpty()) key = MapGeoService::apiKeyFromEnvironment();
    if (key.isEmpty()) return QString();
    QUrl script(QStringLiteral("https://map.qq.com/api/js"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("v"), QStringLiteral("2.exp"));
    query.addQueryItem(QStringLiteral("key"), key);
    script.setQuery(query);
    const QVariantMap data{{QStringLiteral("latitude"), hasLocation_ ? latitude() : 38.914},
                           {QStringLiteral("longitude"), hasLocation_ ? longitude() : 121.614},
                           {QStringLiteral("hasOrigin"), hasLocation_},
                           {QStringLiteral("markers"), markers},
                           {QStringLiteral("route"), points}};
    // Data comes from the database/API. Never interpolate names as executable JS.
    QString json = QString::fromUtf8(QJsonDocument::fromVariant(data).toJson(QJsonDocument::Compact));
    json.replace(QLatin1Char('<'), QStringLiteral("\\u003c"));
    json.replace(QLatin1Char('>'), QStringLiteral("\\u003e"));
    json.replace(QLatin1Char('&'), QStringLiteral("\\u0026"));
    return QStringLiteral(R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>html,body,#map{width:100%;height:100%;margin:0}#error{position:absolute;top:0;background:white;color:#b42318;font:14px sans-serif;padding:8px;z-index:10}</style>
</head><body><div id="map"></div><div id="error">正在加载腾讯地图…</div>
<script src="%1" onerror="document.getElementById('error').textContent='腾讯底图加载失败，请检查网络和 TENCENT_MAP_JS_KEY 授权'"></script>
<script>try{var d=%2;var center=new qq.maps.LatLng(d.latitude,d.longitude);
var map=new qq.maps.Map(document.getElementById('map'),{center:center,zoom:13,mapTypeControl:false});
if(d.hasOrigin)new qq.maps.Marker({position:center,map:map,title:'当前起点'});
var stationBounds=new qq.maps.LatLngBounds(),stationCount=0;
d.markers.forEach(function(s){if(typeof s.lat==='number'&&typeof s.lng==='number'&&isFinite(s.lat)&&isFinite(s.lng)&&Math.abs(s.lat)<=90&&Math.abs(s.lng)<=180){
var p=new qq.maps.LatLng(s.lat,s.lng);var marker=new qq.maps.Marker({position:p,map:map,title:s.label||''});
stationBounds.extend(p);stationCount++;
qq.maps.event.addListener(marker,'click',function(){window.location.href='charging-station://select/'+encodeURIComponent(String(s.id));});}});
if(stationCount>1&&!d.hasOrigin)map.fitBounds(stationBounds);
else if(stationCount===1&&!d.hasOrigin)map.setCenter(stationBounds.getCenter());
if(d.route.length>1){var path=d.route.map(function(p){return new qq.maps.LatLng(p[0],p[1]);});
new qq.maps.Polyline({map:map,path:path,strokeColor:'#00B578',strokeWeight:6,strokeOpacity:0.9});
new qq.maps.Marker({position:path[path.length-1],map:map,title:'目标电站'});
var bounds=new qq.maps.LatLngBounds();path.forEach(function(p){bounds.extend(p);});map.fitBounds(bounds);}
document.getElementById('error').style.display='none';
}catch(e){document.getElementById('error').textContent='腾讯地图初始化失败，请检查 JavaScript 地图密钥授权';}</script></body></html>)HTML")
        .arg(script.toString(QUrl::FullyEncoded).toHtmlEscaped(), json);
}

} // namespace charging::qml
