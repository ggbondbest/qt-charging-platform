#include "services/station/station_query_service.h"

#include "charging/common/model/model_json.h"
#include "network/client_connection.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

namespace charging::client::services::station {

namespace {

// 模拟网络延迟：驱动页面“加载中”状态，接口就绪后走真实通道不再有延迟。
constexpr int kMockLatencyMs = 400;

// 站点查询接口就绪前的演示数据（需求矩阵允许模拟渲染）。金额单位为分。
StationList mockStations()
{
    auto make = [](qint64 id, const char* code, const char* name, const char* address,
                   double lat, double lng, int priceCents, int total, int available,
                   int distanceMeters) {
        charging::model::Station station;
        station.id = id;
        station.code = QString::fromUtf8(code);
        station.name = QString::fromUtf8(name);
        station.address = QString::fromUtf8(address);
        station.latitude = lat;
        station.longitude = lng;
        station.priceCentsPerKwh = priceCents;
        station.status = charging::model::StationStatus::Active;
        station.totalChargers = total;
        station.availableChargers = available;
        return StationListItem{station, distanceMeters};
    };

    return {
        make(1, "SZ-KEY-01", "科技园充电驿站", "南山区科苑南路 1012 号", 22.5412, 113.9430, 120,
             10, 3, 850),
        make(2, "SZ-SDU-02", "深大北门超充站", "南山区深圳大学北门旁", 22.5376, 113.9450, 138, 6,
             1, 1300),
        make(3, "SZ-NSZ-03", "南山智造充电站", "南山区智园二期地下 B1", 22.5451, 113.9505, 98, 8,
             5, 2100),
        make(4, "SZ-BHZ-04", "滨海之窗慢充站", "南山区滨海大道 2009 号", 22.5294, 113.9369, 86, 4,
             0, 2650),
        make(5, "SZ-HTC-05", "后海城市广场站", "南山区后海滨路 3099 号 B2", 22.5208, 113.9380,
             105, 12, 7, 3200),
    };
}

bool matchesKeyword(const StationListItem& item, const QString& keyword)
{
    if (keyword.isEmpty()) {
        return true;
    }
    return item.station.name.contains(keyword, Qt::CaseInsensitive)
        || item.station.address.contains(keyword, Qt::CaseInsensitive);
}

} // namespace

StationQueryService::StationQueryService(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<StationList>("charging::client::services::station::StationList");
}

void StationQueryService::setConnection(charging::client::network::ClientConnection* connection)
{
    if (connection_ == connection) {
        return;
    }
    connection_ = connection;
    if (connection_ != nullptr) {
        connect(connection_, &network::ClientConnection::responseReceived, this,
                &StationQueryService::handleResponse);
        connect(connection_, &network::ClientConnection::requestFailed, this,
                &StationQueryService::handleRequestFailure);
    }
}

void StationQueryService::setLiveMode(bool enabled)
{
    liveMode_ = enabled;
}

bool StationQueryService::liveMode() const
{
    return liveMode_;
}

void StationQueryService::setSimulateFailure(bool simulate)
{
    simulateFailure_ = simulate;
}

bool StationQueryService::isQueryPending() const
{
    return !pendingRequestId_.isEmpty();
}

void StationQueryService::search(const QString& keyword)
{
    if (isQueryPending()) {
        // 真实通道在途：不叠加请求（页面筛选是本地投影，不会走到这里）。
        return;
    }
    pendingKeyword_ = keyword.trimmed();
    emit queryStarted();

    if (liveMode_ && connection_ != nullptr) {
        QJsonObject data;
        data.insert(QStringLiteral("keyword"), pendingKeyword_);
        pendingRequestId_ = connection_->sendRequest(
            QString::fromLatin1(charging::protocol::request_type::kGetStations), data);
        return;
    }

    // 模拟通道：延迟后本地过滤返回。
    QTimer::singleShot(kMockLatencyMs, this, &StationQueryService::finishMockQuery);
}

void StationQueryService::finishMockQuery()
{
    const QString keyword = pendingKeyword_;
    pendingKeyword_.clear();

    if (simulateFailure_) {
        emit queryFailed(tr("站点查询服务暂时不可用，请稍后重试"));
        return;
    }

    StationList results;
    for (const auto& item : mockStations()) {
        if (matchesKeyword(item, keyword)) {
            results.append(item);
        }
    }
    emit querySucceeded(results);
}

void StationQueryService::handleResponse(const charging::protocol::ResponseEnvelope& response)
{
    if (response.requestId != pendingRequestId_
        || response.type
            != QString::fromLatin1(charging::protocol::request_type::kGetStations)) {
        return;
    }
    pendingRequestId_.clear();

    if (!response.success) {
        const QString message = response.error.message.isEmpty()
            ? tr("站点查询失败，请稍后重试")
            : response.error.message;
        emit queryFailed(message);
        return;
    }

    StationList results;
    const QJsonArray stations = response.data.value(QStringLiteral("stations")).toArray();
    for (const auto& value : stations) {
        const QJsonObject object = value.toObject();
        charging::model::Station station;
        QString parseError;
        if (!charging::model::fromJson(object, &station, &parseError)) {
            emit queryFailed(tr("站点数据解析失败：%1").arg(parseError));
            return;
        }
        // 真实接口暂未返回距离时保持 -1，UI 显示“--”。
        const int distanceMeters =
            object.value(QStringLiteral("distanceMeters")).toInt(-1);
        results.append(StationListItem{station, distanceMeters});
    }
    emit querySucceeded(results);
}

void StationQueryService::handleRequestFailure(const QString& requestId, const QString& errorCode,
                                               const QString& message)
{
    if (requestId != pendingRequestId_) {
        return;
    }
    Q_UNUSED(errorCode)
    pendingRequestId_.clear();
    emit queryFailed(message.isEmpty() ? tr("网络异常，站点查询失败") : message);
}

} // namespace charging::client::services::station
