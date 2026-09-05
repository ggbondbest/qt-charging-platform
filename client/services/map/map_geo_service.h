#pragma once

#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

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
    NoApiKey,     // 未配置 CHARGING_TENCENT_MAP_KEY（不发起任何请求）
    Network,      // DNS/连接/SSL 等传输层失败
    Timeout,      // 请求超时无响应
    RateLimited,  // 配额/并发限流（status 120/121、HTTP 429/403）
    InvalidKey,   // 密钥无效或未授权接口（status 111/310/311/312）
    BadResponse,  // 非 JSON / 缺少 result / 其它业务错误
};

// 中文短文案，供页面提示直接使用。
QString mapErrorMessage(MapError error);

// 距离矩阵元素：用户位置 → 某目的地的行驶距离（米）与时长（秒）。
// 注意口径：矩阵 duration 单位是秒，路线规划 duration 单位是分钟。
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

// 路线规划结果：总距离（米）、总时长（分钟）、分段步骤。
struct RouteResult
{
    int distanceMeters = -1;
    int durationMinutes = -1;
    QVector<RouteStep> steps;
};

// 腾讯地图 WebService 请求工具类（成员 2，任务 #17 地图接入）。
//
// 设计口径（队友操作指引）：
// - 密钥仅经环境变量 TENCENT_MAP_API_KEY 注入（兼容旧名
//   CHARGING_TENCENT_MAP_KEY），绝不入库、绝不打印；
//   若控制台开启签名校验，另配 TENCENT_MAP_SECRET_KEY（旧名
//   CHARGING_TENCENT_MAP_SECRET，SK），本类自动
//   按官方规则附带 sig 参数。未配置 key 时**不发起任何请求**，异步回
//   NoApiKey，由页面走模拟数据兜底——兜底策略归消费方（确认页推荐时段 /
//   导航页模拟路线），本类只报告成败，不含预约业务语义。
// - 接口对：距离矩阵 ws/distance/v1/matrix（用户→站点行驶距离/时长，
//   供预约推荐时段）；驾车路线规划 ws/direction/v1/driving（导航页路线，
//   真实响应结构 result.routes[0]）；逆地理编码 ws/geocoder/v1（坐标转
//   地址文本，可选）。
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
    bool hasUsableKey() const;              // 构造时缓存；false = 纯模拟模式

    // 用户（出发）位置：与站点地图面板同口径的南山区演示中心；
    // 真实定位能力就绪后经 setUserLocation 注入，页面零改动。
    void setUserLocation(LatLng location);
    LatLng userLocation() const;

    // 发起请求（返回 requestId，自增，用于过滤过期回调）。
    quint64 requestDistanceMatrix(const QVector<LatLng>& destinations);
    quint64 requestDrivingRoute(LatLng from, LatLng to);
    // 点位逆地理（任务书可选接口）：坐标 → 地址文本（如"广东省深圳市
    // 南山区科兴路"）；失败由消费方回落站点名等模拟口径。
    quint64 requestReverseGeocode(LatLng location);

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

private:
    enum class Kind { Matrix, Route, Geocoder };

    quint64 startRequest(Kind kind, const QVector<LatLng>& destinations, LatLng origin);
    void sendRequest(quint64 requestId, Kind kind, const QString& path,
                     const QMap<QString, QString>& params);
    void emitFailure(quint64 requestId, Kind kind, MapError error);
    // 按官方签名规则：MD5(path + "?" + 参数按 key 排序拼接 + SK)，小写十六进制。
    QString makeSignature(const QString& path, const QMap<QString, QString>& params,
                          const QString& secretKey) const;

    QNetworkAccessManager* network_ = nullptr;
    QString apiKey_;
    QString secretKey_; // 可为空 = 控制台未开签名校验
    bool hasKey_ = false;
    QString endpointBase_ = QStringLiteral("https://apis.map.qq.com/ws");
    int timeoutMsec_ = 5000;
    quint64 nextRequestId_ = 1;
    LatLng userLocation_{22.541, 113.943}; // 演示城市位置（南山区）
};

} // namespace charging::client::services::map

Q_DECLARE_METATYPE(charging::client::services::map::DistanceElement)
Q_DECLARE_METATYPE(charging::client::services::map::RouteStep)
Q_DECLARE_METATYPE(charging::client::services::map::RouteResult)
