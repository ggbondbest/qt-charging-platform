#pragma once

#include <QCache>
#include <QElapsedTimer>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

// 文件职责：腾讯地图 WebService 客户端封装（MapGeoService）及结果结构 / 错误
// 枚举——地图接入批（任务 #17）的唯一头文件。消费方：widgets 侧预约确认页与
// 导航页（C++ 原形信号）、QML 侧 map_bridge/app_bridge（QML 面信号转发）。
// 数据流：页面发起请求拿 requestId → 本类拼参签名发 HTTPS → 解析分类 → 信号
// 回报；密钥与设计口径见下方类注释。

class QNetworkAccessManager;

namespace charging::client::services::map {

// 经纬度（腾讯 WebService 口径：十进制度，纬度在前逗号分隔）。
struct LatLng
{
    double latitude = 0.0;
    double longitude = 0.0;
};

// 接口失败分类：页面兜底文案与提示按类型区分（密钥无效 / 网络不通 / 限流）。
enum class MapError {
    None,
    NoApiKey,     // 环境变量未提供非空 key（不发起任何请求）
    Network,      // DNS/连接/SSL 等传输层失败
    Timeout,      // 请求超时无响应
    RateLimited,  // 短时限流（status 120、HTTP 429）
    InvalidKey,   // 密钥无效或未授权接口（status 111/310/311/312）
    BadResponse,  // 非 JSON / 缺少 result / 其它业务错误
    QuotaExhausted, // 每日额度耗尽（status 121），不得自动重试
    AccessDenied,  // HTTP 403 无明确业务原因，不等同于限流
};

// 中文短文案，供页面提示直接使用。
QString mapErrorMessage(MapError error);

// 距离矩阵元素：用户位置 → 某目的地的行驶距离（米）与时长（秒）。
// 注意口径：矩阵 duration 单位是秒，路线规划 duration 单位是分钟。
// -1 哨兵 = 响应缺该字段（未知），与真实 0 米/0 秒可区分，页面判负值再使用。
struct DistanceElement
{
    int distanceMeters = -1;
    int durationSeconds = -1;
};

// 驾车路线规划的单段步骤（instruction 为腾讯返回的转向文案）。
struct RouteStep
{
    QString instruction;
    int distanceMeters = 0;
};

// 路线规划结果：总距离（米）、总时长（分钟）、分段步骤、坐标折线。
// polyline 已由解析器完成增量解码（绝对经纬度，起点=from）；接口未给
// 或解码失败时为空；正式页面应报告错误，不能生成模拟折线。
struct RouteResult
{
    int distanceMeters = -1;
    int durationMinutes = -1;
    QVector<RouteStep> steps;
    QVector<LatLng> polyline;
};

// 腾讯地图 WebService 请求工具类（成员 2，任务 #17 地图接入）。
//
// 设计口径（队友操作指引）：
// - 密钥只读取环境变量 TENCENT_MAP_API_KEY / 旧名 CHARGING_TENCENT_MAP_KEY；
//   不读取仓库或用户目录中的配置文件，不内置共享 Key，不自动选择演示密钥。
//   生产请求端点固定为 https://apis.map.qq.com/ws，配置文件不能重定向凭证。
//   若控制台开启签名校验，另配 TENCENT_MAP_SECRET_KEY（旧名
//   CHARGING_TENCENT_MAP_SECRET，SK），本类自动
//   按官方规则附带 sig 参数。无 key 时**不发起任何请求**，异步回
//   NoApiKey，正式页面显示配置提示，不生成模拟定位或路线。本类只报告成败，
//   不含预约业务语义。密钥与 URL 零打印。
// - 接口对：距离矩阵 ws/distance/v1/matrix（用户→站点行驶距离/时长，
//   供预约推荐时段）；驾车路线规划 ws/direction/v1/driving（导航页路线，
//   真实响应结构 result.routes[0]）；步行路线 ws/direction/v1/walking；
//   地理编码 ws/geocoder/v1（address 正向地址转坐标，location 逆向）。
// - 异步信号 + requestId 代际：调用方记录返回的 id，收到回调时丢弃过期
//   响应（快速切换站点/页面时旧请求可能后到）。
// - 测试接缝：setEndpointBaseForTesting() 指向进程内假 HTTP 服务，
//   setRequestTimeoutForTesting() 压缩超时；单测永不触真实网络。
class MapGeoService final : public QObject
{
    Q_OBJECT

public:
    explicit MapGeoService(QObject* parent = nullptr);

    // 读环境变量并 trim（不落日志）：TENCENT_MAP_API_KEY 优先，
    // 兼容旧名 CHARGING_TENCENT_MAP_KEY。
    static QString apiKeyFromEnvironment();
    // 兼容存在性查询：任一环境变量名已定义即为真，含空值。
    // 不再参与配置来源选择：不论返回值为何，环境都是唯一 Key 来源。
    static bool environmentKeyAuthoritative();
    // 仅保留源码兼容；不读文件，始终返回空字符串。
    static QString apiKeyFromConfigFile(const QString& configPath = QString());
    static QString baseUrlFromConfigFile(const QString& configPath = QString());
    // 兼容旧调用方：忽略 configPath，分别返回环境 Key 和固定官方端点。
    static QString resolveApiKey(const QString& configPath = QString());
    static QString resolveBaseUrl(const QString& configPath = QString());
    bool hasUsableKey() const;              // 构造时缓存；false = 未配置，禁止联网请求

    // 用户（出发）位置：与站点地图面板同口径的南山区演示中心；
    // 真实定位能力就绪后经 setUserLocation 注入，页面零改动。
    void setUserLocation(LatLng location);
    LatLng userLocation() const;

    // 发起请求（返回 requestId，自增，用于过滤过期回调）。
    quint64 requestDistanceMatrix(const QVector<LatLng>& destinations);
    quint64 requestDrivingRoute(LatLng from, LatLng to);
    quint64 requestWalkingRoute(LatLng from, LatLng to);
    // 正向地理编码：用户输入完整地址，腾讯服务返回坐标。
    quint64 requestForwardGeocode(const QString& address);
    // 点位逆地理（任务书可选接口）：坐标 → 地址文本（如"广东省深圳市
    // 南山区科兴路"）；失败由消费方回落站点名等模拟口径。
    quint64 requestReverseGeocode(LatLng location);

    // —— QML 可调用面（导航页"地图 APP"交互；widgets 消费方仍走上面 C++ 原形） ——
    // 同族 4 标量参数重载：QML 无法构造 LatLng，moc 也只登记 Q_INVOKABLE，
    // 与 C++ 原形互不遮蔽。
    Q_INVOKABLE quint64 requestDrivingRoute(double fromLat, double fromLng,
                                            double toLat, double toLng);
    Q_INVOKABLE quint64 requestReverseGeocodeLatLng(double lat, double lng);
    // IP 粗定位（ws/location/v1/ip）：返回城市级经纬度 + 省市区；
    // 无 key/断网按既有 MapError 口径异步回报，页面自决"定位失败"文案。
    Q_INVOKABLE quint64 requestIpLocation();
    // 地址正地理编码（ws/geocoder/v1?address=）：输入框起点文本 → 坐标。
    Q_INVOKABLE quint64 requestAddressGeocode(const QString& address);
    // 腾讯静态地图图（ws/staticmap/v2）：真实地图瓦片合成 PNG，
    // routePairs=[[lat,lng],…] 画路线（>120 点等距降采样防 414）、
    // markerPairs={latitude,longitude,label} 画标记——label 仅收单 ASCII 字母数字，
    // 中文语义 起→A/终→B（v2 实测 348 红线，详见 .cpp 活体二分注记）。
    // 成功后 qmlStaticMapReady 带临时文件路径（Image.source 直挂）；
    // 失败/无 key 走 qmlStaticMapError；正式页面不能把示意图当作请求成功。
    Q_INVOKABLE quint64 requestStaticMap(double centerLat, double centerLng, int zoom,
                                         int width, int height,
                                         const QVariantList& routePairs,
                                         const QVariantList& markerPairs);
    // 腾讯地图 URI 路线规划页 URL（点击 = 外部浏览器/APP 接力真导航）。
    // URL 内嵌 referer=key（官方要求），由服务端环境注入——调用方勿打印/入库。
    Q_INVOKABLE QString navigationUriUrl(double fromLat, double fromLng, const QString& fromName,
                                         double toLat, double toLng, const QString& toName) const;
    // 用户位置读写（QML 侧标量形）。
    Q_INVOKABLE void setUserLocationLatLng(double lat, double lng)
    {
        setUserLocation(LatLng{lat, lng});
    }
    Q_INVOKABLE QVariantMap userLocationMap() const;
    Q_INVOKABLE bool usable() const { return hasKey_; }

    // 测试接缝（生产不调用）。
    void setEndpointBaseForTesting(const QString& base);
    void setRequestTimeoutForTesting(int msec);

signals:
    void distanceMatrixSucceeded(
        quint64 requestId,
        const QVector<charging::client::services::map::DistanceElement>& elements);
    void distanceMatrixFailed(quint64 requestId, charging::client::services::map::MapError error,
                              const QString& message);
    void routeSucceeded(quint64 requestId,
                        const charging::client::services::map::RouteResult& route);
    void routeFailed(quint64 requestId, charging::client::services::map::MapError error,
                     const QString& message);
    void geocodeSucceeded(quint64 requestId, const QString& address);
    void geocodeFailed(quint64 requestId, charging::client::services::map::MapError error,
                       const QString& message);
    void forwardGeocodeSucceeded(quint64 requestId,
                                 charging::client::services::map::LatLng location,
                                 const QString& address);
    void forwardGeocodeFailed(quint64 requestId, charging::client::services::map::MapError error,
                              const QString& message);

    // —— QML 转发面 ——：上面原生信号载荷是自定义 struct（QML 读不了），
    // 事件在 C++ 信号旁路同点再发 QVariant 形（页面唯一消费面，映射稿 §桥缺口 定形）。
    // polyline=[[lat,lng],…]；steps=[{instruction,distanceMeters}]。
    void qmlRouteReady(quint64 requestId, const QVariantMap& routeMap);
    void qmlRouteError(quint64 requestId, const QString& message);
    void qmlIpLocationReady(quint64 requestId, const QVariantMap& locationMap);
    void qmlIpLocationError(quint64 requestId, const QString& message);
    void qmlGeocodeReady(quint64 requestId, const QVariantMap& pointMap);
    void qmlGeocodeError(quint64 requestId, const QString& message);
    void qmlStaticMapReady(quint64 requestId, const QString& filePath);
    void qmlStaticMapError(quint64 requestId, const QString& message);

private:
    // 2026-09-08 merge：并集——上游新增 WalkingRoute/ForwardGeocoder 与本方
    // QML 面 IpLocation/GeocodeAddress/StaticMap 同存（两侧 switch 各有分支）。
    enum class Kind {
        Matrix, Route, WalkingRoute, Geocoder, ForwardGeocoder,
        IpLocation, GeocodeAddress, StaticMap,   // QML 面新增三族
    };

    quint64 startRequest(Kind kind, const QVector<LatLng>& destinations, LatLng origin);
    void sendRequest(quint64 requestId, Kind kind, const QString& path,
                     const QMap<QString, QString>& params, int attempt = 0);
    void emitFailure(quint64 requestId, Kind kind, MapError error,
                     int httpStatus = 0, int businessStatus = -1);
    struct AddressResult {
        LatLng point;
        QString title;
        QString address;
        qint64 expiresAt = 0;
    };
    struct AddressSubscriber { quint64 id; Kind kind; };
    quint64 startAddressRequest(Kind kind, const QString& address);
    void finishAddressRequest(quint64 requestId, const AddressResult& result);
    void emitAddressResult(quint64 requestId, Kind kind, const AddressResult& result);
    // 按官方签名规则：MD5(path + "?" + 参数按 key 排序拼接 + SK)，小写十六进制。
    QString makeSignature(const QString& path, const QMap<QString, QString>& params,
                          const QString& secretKey) const;

    QNetworkAccessManager* network_ = nullptr;
    QString apiKey_;
    QString secretKey_; // 可为空 = 控制台未开签名校验
    bool hasKey_ = false;
    QString endpointBase_ = QStringLiteral("https://apis.map.qq.com/ws");
    int timeoutMsec_ = 5000;
    // 代际计数：从 1 起、只增不减——0 留给"无效 id"语义（调用方 id>0 才登记），
    // 每个回调带发起时的 id，页面据此丢弃过期响应。
    quint64 nextRequestId_ = 1;
    LatLng userLocation_{22.541, 113.943}; // 演示城市位置（南山区）
    QString lastStaticMapFile_;            // 上一张静态图临时文件（新图落盘时清理）
    // 两个地址解析入口共用：同地址合并在途请求；仅缓存真实成功结果，
    // 最多 32 条、5 分钟、进程内，不写入文件或缓存错误响应。
    // TTL 用 QElapsedTimer 单调毫秒（非挂钟）：系统时间被调整也不会让缓存
    // 提前过期或永不过期。
    QElapsedTimer addressClock_;
    QCache<QString, AddressResult> addressCache_{32};
    QMap<QString, QVector<AddressSubscriber>> addressSubscribers_;
    QMap<quint64, QString> addressOwners_;
};

} // namespace charging::client::services::map

// 自定义 struct 注册元类型：信号载荷要经 QVariant 转换、跨线程/排队连接与
// QML 桥（map_bridge 读信号参数、QSignalSpy value<T>()）才必须声明，
// 缺一则运行期转换失败。
Q_DECLARE_METATYPE(charging::client::services::map::DistanceElement)
Q_DECLARE_METATYPE(charging::client::services::map::LatLng)
Q_DECLARE_METATYPE(charging::client::services::map::RouteStep)
Q_DECLARE_METATYPE(charging::client::services::map::RouteResult)
