// MapGeoService 实现（腾讯地图 WebService 接入批，任务 #17）；类职责与设计
// 口径（环境唯一密钥源、零打印、代际 id、测试缝）见头文件类注释。
// 文件结构：错误分类与文案 → 配置读取（仅环境变量）→ 请求发起（startRequest /
// startAddressRequest）→ 单点发送 sendRequest（拼参/签名/超时看门狗/限流退避）
// → 响应解析（按 Kind 分支回报）→ 签名算法。
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

// ---- 匿名命名空间：业务 status 常量与坐标工具（仅本文件可见，避免污染头文件）----
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

bool validPoint(const LatLng& point)
{
    return std::isfinite(point.latitude) && std::isfinite(point.longitude)
        && point.latitude >= -90 && point.latitude <= 90
        && point.longitude >= -180 && point.longitude <= 180;
}

// 业务 status → MapError 唯一映射点：HTTP 错误体、成功体非 0 status、静态图
// JSON 错误体三处调用共用同一分类，新增 status 码只改这里。
MapError errorFromBusinessStatus(int status)
{
    switch (status) {
    case kHttpRateLimitStatus1:
        return MapError::RateLimited;
    case kHttpRateLimitStatus2:
        return MapError::QuotaExhausted;
    case 110: // 请求来源未授权
    case 112: // IP 未授权
    case 113: // 接口未授权
    case kInvalidKeyStatus1:
    case kInvalidKeyStatus2:
    case kInvalidKeyStatus3:
    case kInvalidKeyStatus4:
        return MapError::InvalidKey;
    default:
        return MapError::BadResponse;
    }
}

// 端点基址是常量而非配置项：凭证随 URL 出网，允许配置文件重定向基址
// 等于把 key/sig 送给任意主机——生产恒为官方域名，仅测试缝可改成员变量。
const QString kDefaultEndpointBase = QStringLiteral("https://apis.map.qq.com/ws");

} // namespace

// ---- 错误枚举 → 页面短文案（固定措辞，绝不混入 URL/响应 message/密钥）----
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
        return QStringLiteral("地图请求过于频繁，请稍后重试");
    case MapError::QuotaExhausted:
        return QStringLiteral("地图接口每日额度已用尽，请查看腾讯控制台地理编码/对应接口额度；稍等或重启不能恢复额度");
    case MapError::AccessDenied:
        return QStringLiteral("地图请求被拒绝，请检查接口授权、签名及网络代理（不代表额度用尽）");
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
    addressClock_.start();
    apiKey_ = apiKeyFromEnvironment();
    endpointBase_ = kDefaultEndpointBase;
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
    // 兼容旧调用方的环境变量存在性查询；无论结果为何，都不会读取配置文件。
    return qEnvironmentVariableIsSet("TENCENT_MAP_API_KEY")
           || qEnvironmentVariableIsSet("CHARGING_TENCENT_MAP_KEY");
}

QString MapGeoService::apiKeyFromConfigFile(const QString& configPath)
{
    Q_UNUSED(configPath);
    return {}; // Retained for source compatibility; file-based credentials are disabled.
}

// "配置文件"桩函数族（本段四个，含紧邻其上的 apiKeyFromConfigFile）：曾经
// 支持从入库 JSON 读 key/baseUrl，该机制已被成员提交 e8546fa 回退（凭证不随
// git 入库、本地文件不得重定向带凭证请求）。函数保留仅为旧调用方源码兼容：
// 一律不读文件，Key 唯一来源是环境变量。
QString MapGeoService::baseUrlFromConfigFile(const QString& configPath)
{
    Q_UNUSED(configPath);
    return {}; // A local file cannot redirect requests carrying the user's key/signature.
}

QString MapGeoService::resolveApiKey(const QString& configPath)
{
    Q_UNUSED(configPath);
    return apiKeyFromEnvironment();
}

QString MapGeoService::resolveBaseUrl(const QString& configPath)
{
    Q_UNUSED(configPath);
    return kDefaultEndpointBase;
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

// 切换端点（仅测试缝）：必须连地址缓存一起清——缓存可能装着真端点的结果，
// 换到假服务器后不得命中串台，保证每条用例零缓存起步。
void MapGeoService::setEndpointBaseForTesting(const QString& base)
{
    addressCache_.clear();
    endpointBase_ = base;
}

// 测试缝：压缩看门狗超时阈值（生产默认 5000ms），让超时分支用例毫秒级收敛。
void MapGeoService::setRequestTimeoutForTesting(int msec)
{
    timeoutMsec_ = msec;
}

// 矩阵/驾车/步行入口：只是 startRequest 公共漏斗的 Kind 薄包装、负责整形参数——
// requestId 分配与本地闸门（无 key/空目的地/非法坐标）的异步回报口径统一在漏斗内。
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
    return startAddressRequest(Kind::ForwardGeocoder, address);
}

quint64 MapGeoService::startAddressRequest(Kind kind, const QString& address)
{
    const quint64 id = nextRequestId_++;
    const QString trimmed = address.trimmed();
    if (!hasKey_ || trimmed.isEmpty() || trimmed.size() > 256) {
        QTimer::singleShot(0, this, [this, id, kind] {
            emitFailure(id, kind,
                        !hasKey_ ? MapError::NoApiKey : MapError::BadResponse);
        });
        return id;
    }
    if (const auto* cached = addressCache_.object(trimmed)) {
        if (cached->expiresAt > addressClock_.elapsed()) {
            // 即使命中缓存也必须异步：调用方先保存 requestId，再接收结果。
            const AddressResult result = *cached;
            QTimer::singleShot(0, this, [this, id, kind, result] {
                emitAddressResult(id, kind, result);
            });
            return id;
        }
        addressCache_.remove(trimmed);
    }
    auto& subscribers = addressSubscribers_[trimmed];
    subscribers.append({id, kind});
    if (subscribers.size() > 1) return id;
    addressOwners_.insert(id, trimmed);
    sendRequest(id, kind, QStringLiteral("/geocoder/v1/"),
                {{QStringLiteral("address"), trimmed}, {QStringLiteral("key"), apiKey_}});
    return id;
}

void MapGeoService::emitAddressResult(quint64 requestId, Kind kind, const AddressResult& result)
{
    if (kind == Kind::ForwardGeocoder) {
        emit forwardGeocodeSucceeded(requestId, result.point, result.title);
    } else {
        emit qmlGeocodeReady(requestId, QVariantMap{
            {QStringLiteral("latitude"), result.point.latitude},
            {QStringLiteral("longitude"), result.point.longitude},
            {QStringLiteral("address"), result.address}});
    }
}

void MapGeoService::finishAddressRequest(quint64 requestId, const AddressResult& result)
{
    const QString address = addressOwners_.take(requestId);
    const auto subscribers = addressSubscribers_.take(address);
    AddressResult cached = result;
    cached.expiresAt = addressClock_.elapsed() + 5 * 60 * 1000;
    addressCache_.insert(address, new AddressResult(cached));
    // 先释放在途状态，再通知页面；失败/成功后都允许下一次正常提交。
    for (const auto& subscriber : subscribers)
        emitAddressResult(subscriber.id, subscriber.kind, cached);
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

// QML 面地址正编：与 requestForwardGeocode 共用 startAddressRequest（同地址在途
// 合流、5 分钟缓存在漏斗里），区别仅在回报走 qmlGeocode* 信号面。
quint64 MapGeoService::requestAddressGeocode(const QString& address)
{
    return startAddressRequest(Kind::GeocodeAddress, address);
}

// 静态出图：本地拼 v2 参数（path/markers 语法细节见下方活体二分注记）后单次
// GET；无 key 闸门与 startRequest 同构——仍返回 requestId、异步回 NoApiKey。
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

// 导航"地图 APP"跳链：URI API 纯本地拼串，不发任何网络请求、无失败回报路径。
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

void MapGeoService::emitFailure(quint64 requestId, Kind kind, MapError error,
                               int httpStatus, int businessStatus)
{
    if (addressOwners_.contains(requestId)) {
        const QString address = addressOwners_.take(requestId);
        const auto subscribers = addressSubscribers_.take(address);
        for (const auto& subscriber : subscribers)
            emitFailure(subscriber.id, subscriber.kind, error, httpStatus, businessStatus);
        return;
    }
    QString message = mapErrorMessage(error);
    // 只透出数字诊断，绝不转发 URL、响应 message 或签名。
    if (httpStatus > 0) message += QStringLiteral(" [HTTP %1]").arg(httpStatus);
    if (businessStatus >= 0) message += QStringLiteral(" [status %1]").arg(businessStatus);
    switch (kind) {
    case Kind::Matrix:
        emit distanceMatrixFailed(requestId, error, message);
        break;
    // Kind → 失败信号分面：C++ 面带类型化 error（消费方按 MapError 分支决策），
    // QML 转发面只送固定文案；逆地理与地址正编共汇 qmlGeocodeError，
    // IpLocation/StaticMap/GeocodeAddress 为 QML 面独有族（无 C++ 对应信号）。
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

// ---- 请求发起：矩阵/驾车/步行/逆地理的公共漏斗（QML 标量重载亦汇入）----
// 本地闸门（无 key / 空目的地 / 非法坐标）与网络失败同构：一律 0ms
// singleShot 异步回报——调用方能无条件"先记 requestId、后收回调"。
quint64 MapGeoService::startRequest(Kind kind, const QVector<LatLng>& destinations,
                                    LatLng origin)
{
    const quint64 requestId = nextRequestId_++;

    if (!hasKey_) {
        // 无密钥：绝不发起请求，异步回 NoApiKey；正式页面显示配置提示。
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
        // mode 是矩阵专属参数：驾车/步行端点以 URL 路径区分出行方式，契约里
        // 没有 mode，多传会偏离官方参数集（它还参与签名拼串），故发送前移除。
        params.remove(QStringLiteral("mode"));
        sendRequest(requestId, kind, kind == Kind::WalkingRoute
                        ? QStringLiteral("/direction/v1/walking/")
                        : QStringLiteral("/direction/v1/driving/"), params);
    }
    return requestId;
}

// ---- 单点发送：全部出口（矩阵/路线/地理编码/IP 定位/静态图）汇聚于此 ----
// 职责：手工拼百分号编码查询串（保 sig 口径）、可选签名、超时看门狗、
// 响应分类（传输层/业务码两级）、限流一次性退避，再按 Kind 分发解析。
void MapGeoService::sendRequest(quint64 requestId, Kind kind, const QString& path,
                                const QMap<QString, QString>& params, int attempt)
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
        if (reply->isFinished()) return;
        reply->setProperty("chargingTimedOut", true);
        reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId, kind, path, params, attempt] {
        // reply 唯一回收点：看门狗 abort() 同样触发 finished 进到这里；
        // deleteLater 排到事件循环下一轮才析构，本回调内仍可安全读 reply 状态。
        reply->deleteLater();
        const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray{};
        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject root = document.isObject() ? document.object() : QJsonObject{};
        const int status = root.value(QStringLiteral("status")).toInt(-1);
        // 传输层判定漏斗：None=无传输层问题；下面的 else-if 顺序即优先级——
        // 自打超时标记最先（区分自发 abort 与网络错），其次信任业务 status
        // （4xx 也可能带可解析错误体），再看 HTTP 状态码，最后才是传输错误。
        MapError transportError = MapError::None;
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->property("chargingTimedOut").toBool())
            transportError = MapError::Timeout;
        else if (httpStatus > 0 && status > 0)
            transportError = errorFromBusinessStatus(status); // 4xx 也必须解析业务错误体
        else if (httpStatus == 429)
            transportError = MapError::RateLimited;
        else if (httpStatus == 403)
            transportError = MapError::AccessDenied;
        else if (httpStatus == 401)
            transportError = MapError::InvalidKey;
        else if (reply->error() != QNetworkReply::NoError)
            transportError = MapError::Network;
        else if (httpStatus < 200 || httpStatus >= 300)
            transportError = MapError::BadResponse;
        if (transportError != MapError::None) {
            // 仅地址解析的短时限流退避一次；日配额、鉴权错误不重试。
            // 服务端要求等待更久/给出日期时直接交给用户，不提前重试。
            if (transportError == MapError::RateLimited && attempt == 0
                && (kind == Kind::ForwardGeocoder || kind == Kind::GeocodeAddress)) {
                const QByteArray retryAfter = reply->rawHeader("Retry-After").trimmed();
                bool numeric = false;
                const int seconds = retryAfter.toInt(&numeric);
                if (retryAfter.isEmpty() || (numeric && seconds >= 0 && seconds <= 5)) {
                    const int delay = retryAfter.isEmpty() ? 1100 : qMax(1100, seconds * 1000);
                    QTimer::singleShot(delay, this, [this, requestId, kind, path, params] {
                        sendRequest(requestId, kind, path, params, 1);
                    });
                    return;
                }
            }
            emitFailure(requestId, kind, transportError, httpStatus, status);
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

        if (!document.isObject()) {
            emitFailure(requestId, kind, MapError::BadResponse, httpStatus);
            return;
        }

        if (status != 0) {
            // 只透出数字状态码，不将可能含密钥的原始 message 交给页面。
            emitFailure(requestId, kind, errorFromBusinessStatus(status), httpStatus, status);
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
                // 缺字段回退 -1 = 未知（真实读数可为 0），消费方判负值再使用。
                elements.append(DistanceElement{
                    object.value(QStringLiteral("distance")).toInt(-1),
                    object.value(QStringLiteral("duration")).toInt(-1)});
            }
            emit distanceMatrixSucceeded(requestId, elements);
        } else if (kind == Kind::ForwardGeocoder || kind == Kind::GeocodeAddress) {
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
            finishAddressRequest(requestId, AddressResult{point,
                result.value(QStringLiteral("title")).toString(),
                result.value(QStringLiteral("address")).toString(), 0});
        } else if (kind == Kind::Geocoder) {
            const QString address = result.value(QStringLiteral("address")).toString();
            // 空 address = 未命中：报 BadResponse 让消费方回落站点名等模拟口径，
            // 不发空文本成功信号。
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
            // (0,0) 双闸：字段缺失（toDouble 默认 0）与接口定位不到返回的
            // 零坐标一并拦下——按"定位失败"回报，页面自决文案。
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

// ---- 签名：官方规则 MD5(path + "?" + key 升序原文参数拼串 + SK) ----
// 两处口径钉死：QMap 迭代天然按 key 升序（拼串免排序）；用编码前原值
//（与查询串各自独立计算，测试逐字节锚见 tst requestQueryMatchesTencentContract）。
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
