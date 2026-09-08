#include "map_geo_service.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include <limits>

namespace charging::client::services::map {

namespace {

#ifdef CHARGING_MAP_CONFIG_FILE
constexpr char kMapConfigFile[] = CHARGING_MAP_CONFIG_FILE;
#else
constexpr char kMapConfigFile[] = "";   // 库外目标未注入宏（不该发生；空=不读文件）
#endif

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

bool validPoint(const LatLng& point)
{
    return std::isfinite(point.latitude) && std::isfinite(point.longitude)
        && point.latitude >= -90 && point.latitude <= 90
        && point.longitude >= -180 && point.longitude <= 180;
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

// git 托管配置文件读取（2026-09-08 key 入库批）：解析失败/文件缺失静默回空对象。
QJsonObject readMapConfig(const QString& configPath)
{
    QString path = configPath;
    if (path.isEmpty())
        path = QString::fromLatin1(kMapConfigFile);
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

const QString kDefaultEndpointBase = QStringLiteral("https://apis.map.qq.com/ws");

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
    apiKey_ = resolveApiKey();
    endpointBase_ = resolveBaseUrl();
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

bool MapGeoService::environmentKeyAuthoritative()
{
    // "已定义即权威"（含空串）：测试 initTestCase 的 qputenv(name,"") 与
    // ctest ENVIRONMENT 注入都落在此分支 → 配置文件永不生效，零真实请求。
    return qEnvironmentVariableIsSet("TENCENT_MAP_API_KEY")
           || qEnvironmentVariableIsSet("CHARGING_TENCENT_MAP_KEY");
}

QString MapGeoService::apiKeyFromConfigFile(const QString& configPath)
{
    return readMapConfig(configPath).value(QStringLiteral("tencentMapKey")).toString().trimmed();
}

QString MapGeoService::baseUrlFromConfigFile(const QString& configPath)
{
    QString base = readMapConfig(configPath).value(QStringLiteral("baseUrl")).toString().trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);   // 拼接点 endpointBase_+path（path 以 / 开头），去尾斜杠防双斜杠
    return base;
}

QString MapGeoService::resolveApiKey(const QString& configPath)
{
    return environmentKeyAuthoritative() ? apiKeyFromEnvironment()
                                         : apiKeyFromConfigFile(configPath);
}

QString MapGeoService::resolveBaseUrl(const QString& configPath)
{
    if (environmentKeyAuthoritative())
        return kDefaultEndpointBase;
    const QString base = baseUrlFromConfigFile(configPath);
    return base.isEmpty() ? kDefaultEndpointBase : base;
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

quint64 MapGeoService::requestWalkingRoute(LatLng from, LatLng to)
{
    return startRequest(Kind::WalkingRoute, {to}, from);
}

quint64 MapGeoService::requestForwardGeocode(const QString& address)
{
    const quint64 id = nextRequestId_++;
    const QString trimmed = address.trimmed();
    if (!hasKey_ || trimmed.isEmpty() || trimmed.size() > 256) {
        QTimer::singleShot(0, this, [this, id] {
            emitFailure(id, Kind::ForwardGeocoder,
                        !hasKey_ ? MapError::NoApiKey : MapError::BadResponse);
        });
        return id;
    }
    sendRequest(id, Kind::ForwardGeocoder, QStringLiteral("/geocoder/v1/"),
                {{QStringLiteral("address"), trimmed}, {QStringLiteral("key"), apiKey_}});
    return id;
}

quint64 MapGeoService::requestReverseGeocode(LatLng location)
{
    return startRequest(Kind::Geocoder, {location}, location);
}

// —— QML 面实现 ——（无 key 一律走 emitFailure 异步 NoApiKey，不发请求）

quint64 MapGeoService::requestDrivingRoute(double fromLat, double fromLng,
                                           double toLat, double toLng)
{
    return requestDrivingRoute(LatLng{fromLat, fromLng}, LatLng{toLat, toLng});
}

quint64 MapGeoService::requestReverseGeocodeLatLng(double lat, double lng)
{
    return requestReverseGeocode(LatLng{lat, lng});
}

QVariantMap MapGeoService::userLocationMap() const
{
    return QVariantMap{
        {QStringLiteral("latitude"), userLocation_.latitude},
        {QStringLiteral("longitude"), userLocation_.longitude},
    };
}

quint64 MapGeoService::requestIpLocation()
{
    const quint64 requestId = nextRequestId_++;
    if (!hasKey_) {
        QTimer::singleShot(0, this, [this, requestId] {
            emitFailure(requestId, Kind::IpLocation, MapError::NoApiKey);
        });
        return requestId;
    }
    QMap<QString, QString> params;
    params.insert(QStringLiteral("key"), apiKey_);
    sendRequest(requestId, Kind::IpLocation, QStringLiteral("/location/v1/ip/"), params);
    return requestId;
}

quint64 MapGeoService::requestAddressGeocode(const QString& address)
{
    const quint64 requestId = nextRequestId_++;
    if (!hasKey_) {
        QTimer::singleShot(0, this, [this, requestId] {
            emitFailure(requestId, Kind::GeocodeAddress, MapError::NoApiKey);
        });
        return requestId;
    }
    const QString trimmed = address.trimmed();
    if (trimmed.isEmpty()) {
        QTimer::singleShot(0, this, [this, requestId] {
            emitFailure(requestId, Kind::GeocodeAddress, MapError::BadResponse);
        });
        return requestId;
    }
    QMap<QString, QString> params;
    params.insert(QStringLiteral("key"), apiKey_);
    params.insert(QStringLiteral("address"), trimmed);   // 中文经 sendRequest 统一百分号编码
    sendRequest(requestId, Kind::GeocodeAddress, QStringLiteral("/geocoder/v1/"), params);
    return requestId;
}

quint64 MapGeoService::requestStaticMap(double centerLat, double centerLng, int zoom,
                                        int width, int height,
                                        const QVariantList& routePairs,
                                        const QVariantList& markerPairs)
{
    const quint64 requestId = nextRequestId_++;
    if (!hasKey_) {
        QTimer::singleShot(0, this, [this, requestId] {
            emitFailure(requestId, Kind::StaticMap, MapError::NoApiKey);
        });
        return requestId;
    }
    // 坐标提取：兼容 [lat,lng] 数组形与 {latitude,longitude} 对象形。
    auto pairLatLng = [](const QVariant& value) {
        const QVariantList list = value.toList();
        if (list.size() >= 2) {
            return LatLng{list.at(0).toDouble(), list.at(1).toDouble()};
        }
        const QVariantMap map = value.toMap();
        return LatLng{map.value(QStringLiteral("latitude")).toDouble(),
                      map.value(QStringLiteral("longitude")).toDouble()};
    };
    QMap<QString, QString> params;
    params.insert(QStringLiteral("key"), apiKey_);
    params.insert(QStringLiteral("center"), formatLatLng(LatLng{centerLat, centerLng}));
    params.insert(QStringLiteral("zoom"), QString::number(qBound(1, zoom, 18)));
    params.insert(QStringLiteral("size"), QStringLiteral("%1*%2")
                      .arg(qBound(64, width, 1024)).arg(qBound(64, height, 1024)));
    params.insert(QStringLiteral("scale"), QStringLiteral("2"));   // 高清（Retina）出图
    if (!routePairs.isEmpty()) {
        // v2 活体二分（2026-09-08）：复数 `paths=` 逗号式被 v2 **静默忽略**
        // （出图与无参基线逐字节相同——线根本没画）；真正画线的是单数
        // `path=color:0xRRGGBB|lat,lng|lat,lng|…` kv 管道式。长折线降采样保留：
        // 京→深数千点全量注入曾把 URL 冲到几十 KB → 414 URI Too Long（活体案，
        // 假 HTTP 短折线撞不到）。等距步长 ≤120 点、首尾必含；≤120 点时步长 1。
        constexpr int kMaxPathPoints = 120;
        const int total = routePairs.size();
        const int stride = total > kMaxPathPoints ? (total + kMaxPathPoints - 1) / kMaxPathPoints : 1;
        QStringList routePoints{QStringLiteral("color:0x00B578")};
        for (int index = 0; index < total; index += stride) {
            routePoints << formatLatLng(pairLatLng(routePairs.at(index)));
        }
        if ((total - 1) % stride != 0) {
            routePoints << formatLatLng(pairLatLng(routePairs.at(total - 1)));
        }
        params.insert(QStringLiteral("path"), routePoints.join(QLatin1Char('|')));
    }
    if (!markerPairs.isEmpty()) {
        // v2 活体二分：markers = `color:0xRRGGBB|label:单ASCII字符|lat,lng` kv 管道式，
        // 多标记走 markers1/markers2…编号参数（单标记用 markers）。Google 逗号式、
        // opacity 段、中文 label 各自独立触发 348 请求参数非法。中文语义标签映射
        // 起→A、终→B；单 ASCII 字母数字原样透传；其余省略 label（只画点不炸请求）。
        static const QHash<QString, QString> kLabelGlyphs = {
            {QStringLiteral("起"), QStringLiteral("A")},
            {QStringLiteral("终"), QStringLiteral("B")},
        };
        const int markerTotal = markerPairs.size();
        int ordinal = 0;
        for (const QVariant& marker : markerPairs) {
            ++ordinal;
            const QVariantMap map = marker.toMap();
            QStringList spec{QStringLiteral("color:0x00A46C")};
            const QString label = map.value(QStringLiteral("label")).toString();
            const QString mapped = kLabelGlyphs.value(label);
            if (!mapped.isEmpty()) {
                spec << QStringLiteral("label:") + mapped;
            } else if (label.size() == 1 && label.at(0).unicode() < 0x80
                       && label.at(0).isLetterOrNumber()) {
                spec << QStringLiteral("label:") + label;
            }
            spec << formatLatLng(LatLng{map.value(QStringLiteral("latitude")).toDouble(),
                                        map.value(QStringLiteral("longitude")).toDouble()});
            params.insert(markerTotal == 1 ? QStringLiteral("markers")
                                           : QStringLiteral("markers%1").arg(ordinal),
                          spec.join(QLatin1Char('|')));
        }
    }
    sendRequest(requestId, Kind::StaticMap, QStringLiteral("/staticmap/v2/"), params);
    return requestId;
}

QString MapGeoService::navigationUriUrl(double fromLat, double fromLng, const QString& fromName,
                                        double toLat, double toLng, const QString& toName) const
{
    // URI API 的 referer 必填 = key；无 key 返回空串，页面隐藏跳转按钮。
    if (!hasKey_) {
        return QString();
    }
    const auto enc = [](const QString& text) {
        return QString::fromLatin1(QUrl::toPercentEncoding(text));
    };
    QString url = QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan?type=drive");
    if (!fromName.isEmpty()) {
        url += QStringLiteral("&from=%1").arg(enc(fromName));
    }
    url += QStringLiteral("&fromcoord=%1,%2")
               .arg(fromLat, 0, 'f', 6).arg(fromLng, 0, 'f', 6);
    if (!toName.isEmpty()) {
        url += QStringLiteral("&to=%1").arg(enc(toName));
    }
    url += QStringLiteral("&tocoord=%1,%2")
               .arg(toLat, 0, 'f', 6).arg(toLng, 0, 'f', 6);
    url += QStringLiteral("&referer=%1").arg(apiKey_);   // 含 key：调用方绝不打印/入库
    return url;
}

void MapGeoService::emitFailure(quint64 requestId, Kind kind, MapError error)
{
    const QString message = mapErrorMessage(error);
    switch (kind) {
    case Kind::Matrix:
        emit distanceMatrixFailed(requestId, error, message);
        break;
    case Kind::Route:
    case Kind::WalkingRoute:
        emit routeFailed(requestId, error, message);
        emit qmlRouteError(requestId, message);   // QML 转发面同点回报
        break;
    case Kind::Geocoder:
        emit geocodeFailed(requestId, error, message);
        emit qmlGeocodeError(requestId, message);
        break;
    case Kind::IpLocation:
        emit qmlIpLocationError(requestId, message);
        break;
    case Kind::GeocodeAddress:
        emit qmlGeocodeError(requestId, message);
        break;
    case Kind::StaticMap:
        emit qmlStaticMapError(requestId, message);
        break;
    case Kind::ForwardGeocoder:
        emit forwardGeocodeFailed(requestId, error, message);
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

    bool coordinatesValid = validPoint(origin);
    for (const auto& point : destinations) coordinatesValid = coordinatesValid && validPoint(point);
    if (destinations.isEmpty() || !coordinatesValid) {
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
        params.remove(QStringLiteral("mode"));
        sendRequest(requestId, kind, kind == Kind::WalkingRoute
                        ? QStringLiteral("/direction/v1/walking/")
                        : QStringLiteral("/direction/v1/driving/"), params);
    }
    return requestId;
}

void MapGeoService::sendRequest(quint64 requestId, Kind kind, const QString& path,
                                const QMap<QString, QString>& params)
{
    // 2026-09-08 merge：查询串保留手工百分号编码而非上游 QUrlQuery——keep set
    // 含 : ; | 等，静态图 path/markers 测试锚（color:0x00B578|22.541000,...）要求
    // 逐字节；QUrlQuery 会把 ':' '|' 编成 %3A 破锚。sig 路径则采纳上游口径
    // = baseUrl 自身路径段 + path（官方规则即完整 URI path，测试锚
    // /ws/distance/v1/matrix/?...）。
    QString query;
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        if (!query.isEmpty()) {
            query += QLatin1Char('&');
        }
        // 百分号编码：中文地址/markers 等必须编码；keep set 保住坐标/静态图
        // 语法字符（, . : ; | ~ - *），使既有测试锚 from=22.541000,113.943000
        // 逐字节不变。sig 按官方口径对编码前原文计算，故仍用 params 原值。
        query += it.key() + QLatin1Char('=')
            + QString::fromLatin1(
                  QUrl::toPercentEncoding(it.value(), QByteArrayLiteral(",-.:;|~*")));
    }
    if (!secretKey_.isEmpty()) {
        query += QStringLiteral("&sig=")
               + makeSignature(QUrl(endpointBase_).path() + path, params, secretKey_);
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

        if (kind == Kind::StaticMap) {
            // 静态图：成功 = PNG 字节流；失败 = JSON 错误体（status 分类）。
            // 落盘临时文件（上一张随新请求清理），qmlStaticMapReady 回路径。
            if (body.size() > 8 && body.startsWith(QByteArrayLiteral("\x89PNG"))) {
                const QString filePath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                    + QStringLiteral("/charging-staticmap-%1.png").arg(requestId);
                QFile file(filePath);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(body);
                    file.close();
                    if (!lastStaticMapFile_.isEmpty() && lastStaticMapFile_ != filePath) {
                        QFile::remove(lastStaticMapFile_);
                    }
                    lastStaticMapFile_ = filePath;
                    emit qmlStaticMapReady(requestId, filePath);
                } else {
                    emitFailure(requestId, kind, MapError::BadResponse);
                }
            } else {
                const QJsonDocument doc = QJsonDocument::fromJson(body);
                const int status =
                    doc.isObject() ? doc.object().value(QStringLiteral("status")).toInt(-1) : -1;
                emitFailure(requestId, kind, errorFromBusinessStatus(status));
            }
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
        } else if (kind == Kind::ForwardGeocoder) {
            const auto location = result.value(QStringLiteral("location")).toObject();
            if (!location.value(QStringLiteral("lat")).isDouble()
                || !location.value(QStringLiteral("lng")).isDouble()) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            const LatLng point{location.value(QStringLiteral("lat")).toDouble(),
                               location.value(QStringLiteral("lng")).toDouble()};
            if (!validPoint(point)) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            emit forwardGeocodeSucceeded(requestId, point,
                result.value(QStringLiteral("title")).toString());
        } else if (kind == Kind::Geocoder) {
            const QString address = result.value(QStringLiteral("address")).toString();
            if (address.isEmpty()) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            emit geocodeSucceeded(requestId, address);
            // QML 面（逆地理 requestReverseGeocodeLatLng 走本族）：地址文本。
            emit qmlGeocodeReady(requestId,
                                 QVariantMap{{QStringLiteral("address"), address}});
        } else if (kind == Kind::IpLocation) {
            const QJsonObject location = result.value(QStringLiteral("location")).toObject();
            const double latitude = location.value(QStringLiteral("lat")).toDouble();
            const double longitude = location.value(QStringLiteral("lng")).toDouble();
            if (qFuzzyIsNull(latitude) && qFuzzyIsNull(longitude)) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            const QJsonObject adInfo = result.value(QStringLiteral("ad_info")).toObject();
            emit qmlIpLocationReady(requestId, QVariantMap{
                {QStringLiteral("latitude"), latitude},
                {QStringLiteral("longitude"), longitude},
                {QStringLiteral("province"), adInfo.value(QStringLiteral("province")).toString()},
                {QStringLiteral("city"), adInfo.value(QStringLiteral("city")).toString()}});
        } else if (kind == Kind::GeocodeAddress) {
            const QJsonObject location = result.value(QStringLiteral("location")).toObject();
            const double latitude = location.value(QStringLiteral("lat")).toDouble();
            const double longitude = location.value(QStringLiteral("lng")).toDouble();
            if (qFuzzyIsNull(latitude) && qFuzzyIsNull(longitude)) {
                emitFailure(requestId, kind, MapError::BadResponse);
                return;
            }
            emit qmlGeocodeReady(requestId, QVariantMap{
                {QStringLiteral("latitude"), latitude},
                {QStringLiteral("longitude"), longitude},
                {QStringLiteral("address"), result.value(QStringLiteral("address")).toString()}});
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
            const double duration = routeObject.value(QStringLiteral("duration")).toDouble(-1);
            route.durationMinutes = std::isfinite(duration) && duration >= 0
                    && duration <= std::numeric_limits<int>::max()
                ? static_cast<int>(std::ceil(duration)) : -1;
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
            // QML 转发面：RouteResult 是自定义 struct，QML 读不了成员，
            // 同点旁路 QVariantMap 形（polyline=[[lat,lng],…]）。
            QVariantList routePointList;
            routePointList.reserve(route.polyline.size());
            for (const auto& point : route.polyline) {
                // 必须显式包 QVariant：否则 append 命中 QVector 的"批量并入"
                // 重载，嵌套结构被拍平成 2N 个标量。
                routePointList.append(QVariant{QVariantList{point.latitude, point.longitude}});
            }
            QVariantList stepList;
            stepList.reserve(route.steps.size());
            for (const auto& step : route.steps) {
                stepList.append(QVariantMap{{QStringLiteral("instruction"), step.instruction},
                                            {QStringLiteral("distanceMeters"), step.distanceMeters}});
            }
            emit qmlRouteReady(requestId, QVariantMap{
                {QStringLiteral("distanceMeters"), route.distanceMeters},
                {QStringLiteral("durationMinutes"), route.durationMinutes},
                {QStringLiteral("polyline"), routePointList},
                {QStringLiteral("steps"), stepList}});
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
