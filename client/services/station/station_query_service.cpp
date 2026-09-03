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
                   int distanceMeters, bool active = true) {
        charging::model::Station station;
        station.id = id;
        station.code = QString::fromUtf8(code);
        station.name = QString::fromUtf8(name);
        station.address = QString::fromUtf8(address);
        station.latitude = lat;
        station.longitude = lng;
        station.priceCentsPerKwh = priceCents;
        station.status = active ? charging::model::StationStatus::Active
                                : charging::model::StationStatus::Inactive;
        station.totalChargers = total;
        station.availableChargers = available;
        return StationListItem{station, distanceMeters};
    };

    // id4 置为离线（Inactive）驱动详情页“站点离线”横幅演示；
    // id6 无桩（totalChargers=0）驱动详情页空数据演示。金额单位为分。
    return {
        make(1, "SZ-KEY-01", "科技园充电驿站", "南山区科苑南路 1012 号", 22.5412, 113.9430, 120,
             10, 3, 850),
        make(2, "SZ-SDU-02", "深大北门超充站", "南山区深圳大学北门旁", 22.5376, 113.9450, 138, 6,
             1, 1300),
        make(3, "SZ-NSZ-03", "南山智造充电站", "南山区智园二期地下 B1", 22.5451, 113.9505, 98, 8,
             5, 2100),
        make(4, "SZ-BHZ-04", "滨海之窗慢充站", "南山区滨海大道 2009 号", 22.5294, 113.9369, 86, 4,
             0, 2650, /*active=*/false),
        make(5, "SZ-HTC-05", "后海城市广场站", "南山区后海滨路 3099 号 B2", 22.5208, 113.9380,
             105, 12, 7, 3200),
        make(6, "SZ-XLH-06", "西丽湖临时站", "南山区丽湖大道 66 号临时场站", 22.5850, 113.9520, 92,
             0, 0, 3800),
    };
}

// 站点充电桩演示数据：各状态数量与站点 availableChargers 严格一致，
// 覆盖 空闲 / 占用(充电中/已预约) / 故障 / 离线，驱动详情页视觉标记。
QVector<charging::model::Charger> mockChargersForStation(const charging::model::Station& station)
{
    using charging::model::Charger;
    using charging::model::ChargerStatus;
    using charging::model::ChargerType;

    struct StatusGroup
    {
        ChargerStatus status;
        int count;
    };

    QVector<StatusGroup> plan;
    switch (station.id) {
    case 1:
        plan = {{ChargerStatus::Available, 3},  {ChargerStatus::Charging, 4},
                {ChargerStatus::Reserved, 1},   {ChargerStatus::Fault, 1},
                {ChargerStatus::Offline, 1}};
        break;
    case 2:
        plan = {{ChargerStatus::Available, 1}, {ChargerStatus::Charging, 3},
                {ChargerStatus::Reserved, 1},  {ChargerStatus::Fault, 1}};
        break;
    case 3:
        plan = {{ChargerStatus::Available, 5}, {ChargerStatus::Charging, 2},
                {ChargerStatus::Fault, 1}};
        break;
    case 4: // 站点整体离线：全部桩离线。
        plan = {{ChargerStatus::Offline, 4}};
        break;
    case 5:
        plan = {{ChargerStatus::Available, 7}, {ChargerStatus::Charging, 3},
                {ChargerStatus::Reserved, 1},  {ChargerStatus::Fault, 1}};
        break;
    case 6: // 空数据演示站点：暂无任何充电桩。
    default:
        plan = {};
        break;
    }

    QVector<Charger> chargers;
    int serial = 0;
    for (const auto& group : plan) {
        for (int i = 0; i < group.count; ++i) {
            ++serial;
            Charger charger;
            charger.id = station.id * 1000 + serial;
            charger.stationId = station.id;
            charger.code =
                QStringLiteral("%1-%2").arg(station.code).arg(serial, 2, 10, QChar('0'));
            charger.type = serial % 3 == 0 ? ChargerType::Slow : ChargerType::Fast;
            charger.powerWatts =
                charger.type == ChargerType::Fast ? (serial % 2 == 0 ? 120000 : 160000) : 7000;
            charger.status = group.status;
            chargers.append(charger);
        }
    }
    return chargers;
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
    qRegisterMetaType<StationDetail>("charging::client::services::station::StationDetail");
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

void StationQueryService::fetchDetail(const charging::model::Station& station,
                                      int distanceMeters)
{
    pendingDetail_.station = station;
    pendingDetail_.distanceMeters = distanceMeters;
    pendingDetail_.chargers.clear();
    pendingDetail_.hasChargerData = false;
    emit detailStarted();

    if (station.id <= 0) {
        // 路由参数非法（无站点 ID / ID 非法）：异步失败，UI 展示错误态 + 返回。
        QTimer::singleShot(0, this, [this]() {
            emit detailFailed(tr("站点信息无效，请返回找站页重新选择"));
        });
        return;
    }

    if (liveMode_ && connection_ != nullptr) {
        QJsonObject data;
        data.insert(QStringLiteral("stationId"), QString::number(station.id));
        pendingDetailRequestId_ = connection_->sendRequest(
            QString::fromLatin1(charging::protocol::request_type::kGetChargers), data);
        return;
    }

    QTimer::singleShot(kMockLatencyMs, this, &StationQueryService::finishMockDetail);
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

void StationQueryService::setMockChargerReserved(qint64 chargerId)
{
    mockChargerOverrides_.insert(chargerId, charging::model::ChargerStatus::Reserved);
}

void StationQueryService::finishMockDetail()
{
    const StationDetail requested = pendingDetail_;

    if (simulateFailure_) {
        emit detailFailed(tr("站点详情服务暂时不可用，请稍后重试"));
        return;
    }

    // 以模拟站点表为准回查（离线/空桩等状态由数据源决定，UI 不伪造）。
    for (const auto& item : mockStations()) {
        if (item.station.id == requested.station.id) {
            StationDetail result = requested;
            result.station = item.station;
            result.chargers = mockChargersForStation(item.station);
            // 应用任务 #17 的“已预约”模拟覆盖，并按覆盖后数据重算空位数。
            int availableNow = 0;
            for (auto& charger : result.chargers) {
                const auto overrideIt = mockChargerOverrides_.find(charger.id);
                if (overrideIt != mockChargerOverrides_.end()) {
                    charger.status = overrideIt.value();
                }
                if (charger.status == charging::model::ChargerStatus::Available) {
                    ++availableNow;
                }
            }
            result.station.availableChargers = availableNow;
            result.hasChargerData = true;
            emit detailSucceeded(result);
            return;
        }
    }
    emit detailFailed(tr("未找到该站点信息，可能已下线，请返回找站页重新选择"));
}

void StationQueryService::handleResponse(const charging::protocol::ResponseEnvelope& response)
{
    const bool isStationQuery = response.requestId == pendingRequestId_
        && response.type
            == QString::fromLatin1(charging::protocol::request_type::kGetStations);
    const bool isDetailQuery = response.requestId == pendingDetailRequestId_
        && response.type
            == QString::fromLatin1(charging::protocol::request_type::kGetChargers);
    if (!isStationQuery && !isDetailQuery) {
        return;
    }

    if (isDetailQuery) {
        pendingDetailRequestId_.clear();
        if (!response.success) {
            const QString message = response.error.message.isEmpty()
                ? tr("站点详情加载失败，请稍后重试")
                : response.error.message;
            emit detailFailed(message);
            return;
        }
        StationDetail result = pendingDetail_;
        const QJsonArray chargers = response.data.value(QStringLiteral("chargers")).toArray();
        for (const auto& value : chargers) {
            charging::model::Charger charger;
            QString parseError;
            if (!charging::model::fromJson(value.toObject(), &charger, &parseError)) {
                emit detailFailed(tr("充电桩数据解析失败：%1").arg(parseError));
                return;
            }
            result.chargers.append(charger);
        }
        result.hasChargerData = true;
        emit detailSucceeded(result);
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
    Q_UNUSED(errorCode)
    if (requestId == pendingDetailRequestId_) {
        pendingDetailRequestId_.clear();
        emit detailFailed(message.isEmpty() ? tr("网络异常，站点详情加载失败") : message);
        return;
    }
    if (requestId != pendingRequestId_) {
        return;
    }
    pendingRequestId_.clear();
    emit queryFailed(message.isEmpty() ? tr("网络异常，站点查询失败") : message);
}

} // namespace charging::client::services::station
