#include "map_geo_service.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace charging::client::services::map {

namespace {

constexpr int kHttpRateLimitStatus1 = 120; // 触发并发/限流
constexpr int kHttpRateLimitStatus2 = 121; // 每日配额超限
constexpr int kInvalidKeyStatus1 = 111;    // 请求源未通过验证（key 无效）
constexpr int kInvalidKeyStatus2 = 310;    // 请求认证密钥失败
constexpr int kInvalidKeyStatus3 = 311;    // 请求密钥格式错误
constexpr int kInvalidKeyStatus4 = 312;    // 没有权限使用接口

// 坐标串：腾讯口径"纬度,经度"，保留 6 位小数。
QString formatLatLng(const LatLng& point)
{
    return QStringLiteral("%1,%2")
        .arg(point.latitude, 0, 'f', 6)
        .arg(point.longitude, 0, 'f', 6);
}

MapError errorFromBusinessStatus(int status)
{
    switch (status) {
    case kHttpRateLimitStatus1:
    case kHttpRateLimitStatus2:
        return MapError::RateLimited;
    case kInvalidKeyStatus1:
    case kInvalidKeyStatus2:
    case kInvalidKeyStatus3:
    case kInvalidKeyStatus4:
        return MapError::InvalidKey;
    default:
        return MapError::BadResponse;
    }
}

} // namespace

QString mapErrorMessage(MapError error)
{
    switch (error) {
    case MapError::None:
        return QStringLiteral("成功");
    case MapError::NoApiKey:
        return QStringLiteral("未配置地图密钥");
    case MapError::Network:
        return QStringLiteral("网络不通");
    case MapError::Timeout:
        return QStringLiteral("接口请求超时");
    case MapError::RateLimited:
        return QStringLiteral("接口调用受限（配额或并发超限）");
    case MapError::InvalidKey:
        return QStringLiteral("密钥无效或未授权该接口");
    case MapError::BadResponse:
        return QStringLiteral("接口返回异常");
    }
    return QStringLiteral("接口返回异常");
}

MapGeoService::MapGeoService(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this))
{
    apiKey_ = apiKeyFromEnvironment();
    // SK 仅当控制台开启签名校验时才需要；为空则请求不带 sig。
    secretKey_ = qEnvironmentVariable("TENCENT_MAP_SECRET_KEY").trimmed();
    if (secretKey_.isEmpty()) {
        secretKey_ = qEnvironmentVariable("CHARGING_TENCENT_MAP_SECRET").trimmed();
    }
    hasKey_ = !apiKey_.isEmpty();
}

QString MapGeoService::apiKeyFromEnvironment()
{
    // 任务书口径：TENCENT_MAP_API_KEY；兼容早期约定的旧名。
    QString key = qEnvironmentVariable("TENCENT_MAP_API_KEY").trimmed();
    if (key.isEmpty()) {
        key = qEnvironmentVariable("CHARGING_TENCENT_MAP_KEY").trimmed();
    }
    return key;
}

bool MapGeoService::hasUsableKey() const
{
    return hasKey_;
}

void MapGeoService::setUserLocation(LatLng location)
{
    userLocation_ = location;
}

LatLng MapGeoService::userLocation() const
{
    return userLocation_;
}

void MapGeoService::setEndpointBaseForTesting(const QString& base)
{
    endpointBase_ = base;
}

void MapGeoService::setRequestTimeoutForTesting(int msec)
{
    timeoutMsec_ = msec;
}

quint64 MapGeoService::requestDistanceMatrix(const QVector<LatLng>& destinations)
{
    return startRequest(Kind::Matrix, destinations, userLocation_);
}

quint64 MapGeoService::requestDrivingRoute(LatLng from, LatLng to)
{
    return startRequest(Kind::Route, {to}, from);
}

quint64 MapGeoService::requestReverseGeocode(LatLng location)
{
    return startRequest(Kind::Geocoder, {location}, location);
}

void MapGeoService::emitFailure(quint64 requestId, Kind kind, MapError error)
{
    const QString message = mapErrorMessage(error);
    switch (kind) {
    case Kind::Matrix:
        emit distanceMatrixFailed(requestId, error, message);
        break;
    case Kind::Route:
        emit routeFailed(requestId, error, message);
        break;
    case Kind::Geocoder:
        emit geocodeFailed(requestId, error, message);
        break;
    }
}

quint64 MapGeoService::startRequest(Kind kind, const QVector<LatLng>& destinations,
                                    LatLng origin)
{
    const quint64 requestId = nextRequestId_++;

    if (!hasKey_) {
        // 无密钥：绝不发起请求，异步回 NoApiKey，页面按模拟数据兜底。
        QTimer::singleShot(0, this, [this, requestId, kind] {
            emitFailure(requestId, kind, MapError::NoApiKey);
        });
        return requestId;
    }

    if (destinations.isEmpty()) {
        QTimer::singleShot(0, this, [this, requestId, kind] {
            emitFailure(requestId, kind, MapError::BadResponse);
        });
        return requestId;
    }

    QStringList destinationParts;
    destinationParts.reserve(destinations.size());
    for (const auto& destination : destinations) {
        destinationParts << formatLatLng(destination);
    }

    QMap<QString, QString> params; // QMap 按 key 升序，正好满足签名拼接口径
    params.insert(QStringLiteral("key"), apiKey_);
    if (kind == Kind::Geocoder) {
        params.insert(QStringLiteral("location"), destinationParts.first());
        sendRequest(requestId, kind, QStringLiteral("/geocoder/v1/"), params);
        return requestId;
    }
    params.insert(QStringLiteral("mode"), QStringLiteral("driving"));
    if (kind == Kind::Matrix) {
        params.insert(QStringLiteral("from"), formatLatLng(origin));
        params.insert(QStringLiteral("to"), destinationParts.join(QLatin1Char(';')));
        sendRequest(requestId, kind, QStringLiteral("/distance/v1/matrix/"), params);
    } else {
        params.insert(QStringLiteral("from"), formatLatLng(origin));
        params.insert(QStringLiteral("to"), destinationParts.first());
        sendRequest(requestId, kind, QStringLiteral("/direction/v1/driving/"), params);
    }
    return requestId;
}

void MapGeoService::sendRequest(quint64 requestId, Kind kind, const QString& path,
                                const QMap<QString, QString>& params)
{
    QString query;
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        if (!query.isEmpty()) {
            query += QLatin1Char('&');
        }
        query += it.key() + QLatin1Char('=') + it.value();
    }
    if (!secretKey_.isEmpty()) {
        query += QStringLiteral("&sig=") + makeSignature(path, params, secretKey_);
    }

    // 注意：URL 含密钥，绝不写入任何日志/错误文案；message 只用固定文案。
    QNetworkRequest request{QUrl(endpointBase_ + path + QLatin1Char('?') + query)};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("qt-charging-platform/1.0"));
    QNetworkReply* reply = network_->get(request);

    // 超时看门狗：到点主动中断并打标，与调用方外部 abort 区分。
    QTimer::singleShot(timeoutMsec_, reply, [reply] {
        reply->setProperty("chargingTimedOut", true);
        reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId, kind] {
        reply->deleteLater();
        QByteArray body;
        MapError transportError = MapError::None;
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (reply->property("chargingTimedOut").toBool()) {
                transportError = MapError::Timeout;
            } else if (httpStatus == 403 || httpStatus == 429) {
                // Qt6 把 4xx 也报成 reply->error()：先按状态码归类限流。
                transportError = MapError::RateLimited;
            } else if (reply->error() == QNetworkReply::SslHandshakeFailedError
                       || reply->error() == QNetworkReply::ProtocolInvalidOperationError) {
                // 运行环境缺少 OpenSSL 等：按网络不通兜底。
                transportError = MapError::Network;
            } else if (reply->error() == QNetworkReply::OperationCanceledError) {
                transportError = MapError::Network;
            } else {
                transportError = MapError::Network;
            }
        } else {
            if (httpStatus == 403 || httpStatus == 429) {
                transportError = MapError::RateLimited;
            } else if (httpStatus < 200 || httpStatus >= 300) {
                transportError = MapError::BadResponse;
            }
            body = reply->readAll();
        }
        if (transportError != MapError::None) {
            emitFailure(requestId, kind, transportError);
            return;
        }

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        const QJsonObject root = document.isObject() ? document.object() : QJsonObject{};
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emitFailure(requestId, kind, MapError::BadResponse);
            return;
        }

        const int status = root.value(QStringLiteral("status")).toInt(-1);
        if (status != 0) {
            // 业务错误：仅透出固定分类文案（message 字段可能是 key 相关提示，
            // 不透传原文，避免敏感内容进 UI/日志）。真实案例：status 121
            // "此key每日调用量已达到上限" → RateLimited 兜底。
            emitFailure(requestId, kind, errorFromBusinessStatus(status));
            return;
        }

        const QJsonObject result = root.value(QStringLiteral("result")).toObject();
        if (kind == Kind::Matrix) {
            QVector<DistanceElement> elements;
            const QJsonArray rows = result.value(QStringLiteral("rows")).toArray();
            if (rows.isEmpty() || !rows.first().isObject()) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            const QJsonArray items =
                rows.first().toObject().value(QStringLiteral("elements")).toArray();
            elements.reserve(items.size());
            for (const auto& item : items) {
                const QJsonObject object = item.toObject();
                elements.append(DistanceElement{
                    object.value(QStringLiteral("distance")).toInt(-1),
                    object.value(QStringLiteral("duration")).toInt(-1)});
            }
            emit distanceMatrixSucceeded(requestId, elements);
        } else if (kind == Kind::Geocoder) {
            const QString address = result.value(QStringLiteral("address")).toString();
            if (address.isEmpty()) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            emit geocodeSucceeded(requestId, address);
        } else {
            // 真实响应结构为 result.routes[0]（含 distance/duration/steps[]，
            // duration 单位=分钟）；旧文档口径 result.mode 保留兼容回退。
            QJsonObject routeObject;
            const QJsonArray routes = result.value(QStringLiteral("routes")).toArray();
            if (!routes.isEmpty() && routes.first().isObject()) {
                routeObject = routes.first().toObject();
            } else {
                routeObject = result.value(QStringLiteral("mode")).toObject();
            }
            if (routeObject.isEmpty()) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            RouteResult route;
            route.distanceMeters = routeObject.value(QStringLiteral("distance")).toInt(-1);
            route.durationMinutes = routeObject.value(QStringLiteral("duration")).toInt(-1);
            const QJsonArray steps = routeObject.value(QStringLiteral("steps")).toArray();
            route.steps.reserve(steps.size());
            for (const auto& item : steps) {
                const QJsonObject object = item.toObject();
                route.steps.append(RouteStep{object.value(QStringLiteral("instruction")).toString(),
                                             object.value(QStringLiteral("distance")).toInt(0)});
            }
            // 坐标折线（官方口径）：前两个元素是首点**绝对度数**（如
            // 50.243916，无需缩放）；其后为整数微度增量，规则
            // coors[i] = coors[i-2] + coors[i]/1e6。部分版本首点也按微度
            // 返回，按量级归一。解码越出中国范围视为脏数据整体置空
            //（防飞线），消费方回落模拟折线。
            const QJsonArray encoded = routeObject.value(QStringLiteral("polyline")).toArray();
            QVector<LatLng> decoded;
            bool polylineSane = true;
            if (encoded.size() >= 2) {
                decoded.reserve(encoded.size() / 2);
                double latitude = encoded.at(0).toDouble();
                double longitude = encoded.at(1).toDouble();
                if (qAbs(latitude) > 1000.0 || qAbs(longitude) > 1000.0) {
                    latitude /= 1e6;
                    longitude /= 1e6;
                }
                polylineSane = latitude >= 15.0 && latitude <= 55.0
                    && longitude >= 73.0 && longitude <= 136.0;
                if (polylineSane) {
                    decoded.append(LatLng{latitude, longitude});
                }
                for (qsizetype i = 2; polylineSane && i + 1 < encoded.size(); i += 2) {
                    latitude += encoded.at(i).toDouble() / 1e6;
                    longitude += encoded.at(i + 1).toDouble() / 1e6;
                    if (latitude < 15.0 || latitude > 55.0 || longitude < 73.0 || longitude > 136.0) {
                        polylineSane = false;
                        break;
                    }
                    decoded.append(LatLng{latitude, longitude});
                }
                if (!polylineSane) {
                    decoded.clear();
                }
            }
            route.polyline = std::move(decoded);
            emit routeSucceeded(requestId, route);
        }
    });
}

QString MapGeoService::makeSignature(const QString& path, const QMap<QString, QString>& params,
                                     const QString& secretKey) const
{
    QString raw = path + QLatin1Char('?');
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        if (it != params.cbegin()) {
            raw += QLatin1Char('&');
        }
        raw += it.key() + QLatin1Char('=') + it.value();
    }
    raw += secretKey;
    return QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Md5).toHex());
}

} // namespace charging::client::services::map
