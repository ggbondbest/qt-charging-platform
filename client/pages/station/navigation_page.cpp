#include "pages/station/navigation_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/toast.h"
#include "pages/station/platform_theme.h"
#include "pages/station/station_map_panel.h"
#include "services/map/map_geo_service.h"

#include <cmath>

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

using services::reservation::ReservationRecord;
using services::reservation::ReservationService;

// 页面局部样式：仅本页生效，不改全局 QSS。
const char* kNavigationPageStyleSheet = R"(
QWidget#navigationPage {
    background: #F7F9FB;
}
QLabel#navigationPageTitle {
    color: #1F2937;
    font-size: 16px;
    font-weight: 700;
}
QLabel#navigationMapPlaceholder {
    color: #6B7280;
    background: #E8EEF4;
    border: 1px dashed #B9C4CF;
    border-radius: 12px;
    font-size: 12px;
    padding: 26px 12px;
}
QLabel#navigationCaptionLabel {
    color: #9AA5B1;
    font-size: 11px;
}
)";

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// 真实地图接入（成员 2 地图接入迭代）：
//
// 1. 路线规划：MapGeoService::requestDrivingRoute（腾讯 WebService
//    ws/direction/v1/driving/，key 经环境变量注入）→ handleRouteResult
//    原地替换距离/时长，caption 切“真实导航路线 · 腾讯地图”；接口异常
//    保持模拟口径 + caption 标注原因 + Toast。见 openRoute/handleRouteResult。
// 2. 地图渲染：WebEngine 构建时本页顶部挂 StationMapPanel 渲染路线
//    polyline（updateRouteMap：真实折线 routes[0].polyline 优先、模拟
//    正弦折线兜底，站点标记 + fitBounds 自动包住视野）；无 WebEngine 或
//    面板降级时保留占位文案（CI 口径不变）。
// 3. 文字步骤模块（迭代 3 移除）：模拟路段文案与真实地理不符，产品口径
//    改为“仅地图可视化展示”，正确距离/时长依赖后端真实站点坐标与用户
//    定位；steps 解析保留在服务层（tst_map_geo_service 锁定），页面不再消费。
// 4. 距离口径统一：确认页距离矩阵结果已写入 ReservationRecord
//    distanceMeters，本页直接消费同一记录。
// ─────────────────────────────────────────────────────────────────────────
NavigationPage::NavigationPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("navigationPage"));
    setStyleSheet(QString::fromLatin1(kNavigationPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("导航前往充电桩"), this);
    titleLabel->setObjectName(QStringLiteral("navigationPageTitle"));
    rootLayout->addWidget(titleLabel);

    auto* caption = new QLabel(
        tr("导航路线为模拟数据 · 腾讯地图路线接口就绪后自动切换真实路线"),
        this);
    caption->setObjectName(QStringLiteral("navigationCaptionLabel"));
    caption->setWordWrap(true);
    captionLabel_ = caption;
    defaultCaptionText_ = caption->text();
    rootLayout->addWidget(caption);

    // 长内容滚动容器：鼠标滚轮上下滚动（规格通用要求）；与顶部地图
    // 组成垂直分栏（见文末 splitter），分隔条可拖动调整地图大小。
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("navigationScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 8, 0);
    contentLayout->setSpacing(10);
    scroll->setWidget(content);

    // 顶部地图区（成员 2 地图渲染迭代）：WebEngine 构建时挂真实腾讯地图
    // 面板渲染路线折线（openRoute/handleRouteResult 经 updateRouteMap 喂
    // 数据），与下方概要卡组成可拖拽垂直分栏；无 WebEngine 或地图未就绪
    // 时保留占位（CI 口径不变，占位仅隐藏、不销毁——页面级测试仍可定位该
    // objectName）。验收修复：WebEngine 构建下构造期面板必然还在异步加载
    // （degraded_ 初值 true），可见性只算一次会让占位在地图渲染成功后永久
    // 残留——接 mapReady（一次性信号）即时隐藏。
#ifdef CHARGING_PLATFORM_HAS_WEBENGINE
    routeMapPanel_ = new StationMapPanel(this);
    routeMapPanel_->setObjectName(QStringLiteral("navigationRouteMapPanel"));
    routeMapPanel_->setMinimumHeight(56); // 与首页面板同口径
#endif
    mapPlaceholderLabel_ = new QLabel(tr("🗺️ 地图未就绪（加载中，或组件/密钥不可用）"), content);
    mapPlaceholderLabel_->setObjectName(QStringLiteral("navigationMapPlaceholder"));
    mapPlaceholderLabel_->setAlignment(Qt::AlignCenter);
    mapPlaceholderLabel_->setVisible(routeMapPanel_ == nullptr || routeMapPanel_->isDegraded());
    if (routeMapPanel_ != nullptr) {
        connect(routeMapPanel_, &StationMapPanel::mapReady, mapPlaceholderLabel_, &QWidget::hide);
    }
    contentLayout->addWidget(mapPlaceholderLabel_);

    // 行程概要卡：目标 / 距离 / 预计到达。
    auto* summaryCard = new Card(content);
    summaryCard->setProperty("isNavigationSummaryCard", true);
    auto* summaryBody = summaryCard->bodyLayout();
    targetLabel_ = new QLabel(summaryCard);
    targetLabel_->setObjectName(QStringLiteral("navigationTargetLabel"));
    targetLabel_->setProperty("role", QStringLiteral("sectionTitle"));
    targetLabel_->setWordWrap(true);
    auto* metricRow = new QHBoxLayout();
    distanceLabel_ = new QLabel(summaryCard);
    distanceLabel_->setObjectName(QStringLiteral("navigationDistanceLabel"));
    distanceLabel_->setProperty("role", QStringLiteral("amountStrong"));
    etaLabel_ = new QLabel(summaryCard);
    etaLabel_->setObjectName(QStringLiteral("navigationEtaLabel"));
    etaLabel_->setProperty("role", QStringLiteral("secondary"));
    etaLabel_->setWordWrap(true);
    metricRow->addWidget(distanceLabel_);
    metricRow->addStretch();
    metricRow->addWidget(etaLabel_);
    summaryBody->addWidget(targetLabel_);
    summaryBody->addLayout(metricRow);
    contentLayout->addWidget(summaryCard);
    contentLayout->addStretch();

    // 路线步骤文字列表模块（迭代 3 产品口径）：文字步骤与真实地理不符
    // （模拟模板拼的路段名并非真实路线），整块删除——本页只做地图路线
    // 可视化展示；距离/时长正确性依赖后端传入真实站点坐标与用户定位，
    // 接口异常时展示模拟占位口径（caption + Toast 提示）。
    //
    // 地图 / 概要卡垂直分栏：拖动分隔条可放大地图（与首页同口径）；
    // 初始地图高对齐找站页（attachToSplitter 统一升档逻辑）。
    auto* navSplitter = new QSplitter(Qt::Vertical, this);
    navSplitter->setObjectName(QStringLiteral("navigationRouteSplitter"));
    navSplitter->setHandleWidth(10);
    if (routeMapPanel_ != nullptr) {
        navSplitter->addWidget(routeMapPanel_);
    }
    navSplitter->addWidget(scroll);
    navSplitter->setStretchFactor(0, 0); // 窗口拉伸增量归内容区
    navSplitter->setStretchFactor(1, 1);
    if (routeMapPanel_ != nullptr) {
        routeMapPanel_->attachToSplitter(navSplitter, 300);
    } else {
        navSplitter->setSizes({0, 480}); // 非 WebEngine：内容区独占
    }
    rootLayout->addWidget(navSplitter, 1);
}

void NavigationPage::setMapService(services::map::MapGeoService* mapService)
{
    if (mapService_ == mapService) {
        return;
    }
    mapService_ = mapService;
    if (mapService_ == nullptr) {
        return;
    }
    connect(mapService_, &services::map::MapGeoService::routeSucceeded, this,
            [this](quint64 requestId, const services::map::RouteResult& route) {
                handleRouteResult(requestId, route);
            });
    connect(mapService_, &services::map::MapGeoService::routeFailed, this,
            [this](quint64 requestId, services::map::MapError, const QString& message) {
                handleRouteFailure(requestId, message);
            });
    connect(mapService_, &services::map::MapGeoService::geocodeSucceeded, this,
            [this](quint64 requestId, const QString& address) {
                handleGeocodeResult(requestId, address);
            });
    // geocodeFailed 静默：可选接口，失败保持“站名·桩编号”模拟口径即可。
}

void NavigationPage::openRoute(const ReservationRecord& record)
{
    record_ = record;
    targetLabel_->setText(tr("前往：%1 · %2（%3）")
                              .arg(record_.stationName, record_.chargerCode, record_.chargerSpec));

    const int distance = qMax(0, record_.distanceMeters);
    distanceLabel_->setText(distance >= 1000
                                ? tr("全程约 %1 km").arg(distance / 1000.0, 0, 'f', 1)
                                : tr("全程约 %1 m").arg(distance));

    // 模拟口径先行渲染（永不空页）：真实路线到达后原地替换。
    // 行驶分钟数与确认页推荐时段同源（recommendSlot 模拟估算）。
    const int travelMinutes =
        ReservationService::recommendSlot(record_.distanceMeters).travelMinutes;
    QString etaText = tr("预计行驶约 %1 分钟").arg(travelMinutes);
    if (record_.startAtUtc.isValid()) {
        etaText += tr(" · 建议 %1 前出发（预约 %2 开始）")
                       .arg(record_.startAtUtc.addSecs(-(travelMinutes + 5) * 60)
                                .toLocalTime()
                                .toString(QStringLiteral("HH:mm")),
                            record_.startAtUtc.toLocalTime().toString(QStringLiteral("HH:mm")));
    }
    etaLabel_->setText(etaText);

    // 真实路线（地图接入）：key 可用且预约记录带站点坐标时异步请求；
    // 复位展示口径并记录代际，过期响应在回调里丢弃。请求在途 caption
    // 显示 loading 态（任务书第 3 条），到达/失败后改为最终口径。
    usingRealRoute_ = false;
    realPolyline_.clear();
    captionLabel_->setText(defaultCaptionText_);
    routeGeneration_ = 0;
    geocodeGeneration_ = 0;
    if (mapService_ != nullptr && mapService_->hasUsableKey() && record_.hasStationLocation) {
        routeGeneration_ = mapService_->requestDrivingRoute(
            mapService_->userLocation(),
            {record_.stationLatitude, record_.stationLongitude});
        captionLabel_->setText(tr("正在加载真实导航路线…"));
        geocodeGeneration_ = mapService_->requestReverseGeocode(
            {record_.stationLatitude, record_.stationLongitude});
    }

    // 地图与文字路线同口径：模拟折线先渲染，真实折线到达后原地替换。
    updateRouteMap();
}

void NavigationPage::handleRouteResult(quint64 requestId,
                                       const services::map::RouteResult& route)
{
    if (requestId != routeGeneration_) {
        return; // 已切页/重复请求：旧响应作废
    }
    routeGeneration_ = 0;

    if (route.distanceMeters >= 0) {
        const int meters = route.distanceMeters;
        distanceLabel_->setText(meters >= 1000
                                    ? tr("全程约 %1 km").arg(meters / 1000.0, 0, 'f', 1)
                                    : tr("全程约 %1 m").arg(meters));
    }
    const int travelMinutes = route.durationMinutes;
    if (travelMinutes > 0) {
        QString etaText = tr("预计行驶约 %1 分钟").arg(travelMinutes);
        if (record_.startAtUtc.isValid()) {
            etaText += tr(" · 建议 %1 前出发（预约 %2 开始）")
                           .arg(record_.startAtUtc.addSecs(-(travelMinutes + 5) * 60)
                                    .toLocalTime()
                                    .toString(QStringLiteral("HH:mm")),
                                record_.startAtUtc.toLocalTime().toString(QStringLiteral("HH:mm")));
        }
        etaLabel_->setText(etaText);
    }

    usingRealRoute_ = true;
    captionLabel_->setText(tr("真实导航路线 · 腾讯地图"));
    // 真实折线（解码失败为空则保持模拟折线口径）。
    realPolyline_.clear();
    for (const auto& point : route.polyline) {
        realPolyline_.append({point.latitude, point.longitude});
    }
    updateRouteMap();
}

void NavigationPage::handleRouteFailure(quint64 requestId, const QString& message)
{
    if (requestId != routeGeneration_) {
        return;
    }
    routeGeneration_ = 0;
    // 保持模拟路线，caption 标注原因 + 一次性非阻塞提示（任务书第 3 条）。
    captionLabel_->setText(tr("导航路线为模拟数据（接口异常：%1）").arg(message));
    realPolyline_.clear(); // 地图同步回落模拟折线
    updateRouteMap();
    Toast::show(window(), tr("地图服务暂不可用（%1），已展示模拟路线").arg(message),
                charging::client::StatusTag::Tone::Warning);
}

void NavigationPage::handleGeocodeResult(quint64 requestId, const QString& address)
{
    if (requestId != geocodeGeneration_) {
        return; // 已切页：过期地址响应作废
    }
    geocodeGeneration_ = 0;
    if (address.isEmpty()) {
        return;
    }
    // “前往”行追加真实地址（逆地理失败则保持站名模拟口径）。
    targetLabel_->setText(tr("前往：%1 · %2（%3）｜%4")
                              .arg(record_.stationName, record_.chargerCode,
                                   record_.chargerSpec, address));
}

QString NavigationPage::distanceText() const
{
    return distanceLabel_->text();
}

QString NavigationPage::etaText() const
{
    return etaLabel_->text();
}

bool NavigationPage::usingRealRoute() const
{
    return usingRealRoute_;
}

void NavigationPage::updateRouteMap()
{
    if (routeMapPanel_ == nullptr) {
        return; // 非 WebEngine 构建：占位文案承担，无数据可喂
    }
    using services::map::LatLng;
    const LatLng from = mapService_ != nullptr
        ? mapService_->userLocation()
        : LatLng{22.541, 113.943}; // 与地图面板默认中心同口径
    LatLng to{from.latitude + 0.020, from.longitude + 0.012};
    if (record_.hasStationLocation) {
        to = {record_.stationLatitude, record_.stationLongitude};
    }

    QVector<MapStationPoint> line;
    if (!realPolyline_.isEmpty()) {
        line.reserve(realPolyline_.size());
        for (const auto& point : realPolyline_) {
            line.append({point.first, point.second, {}});
        }
    } else {
        // 模拟折线：起终点线性插值 + 垂直方向正弦偏移（约 200 米弦高），
        // 接口失败/无坐标时地图永不空白（文案步骤模块已移除，可视化兜底）。
        constexpr int kSegments = 10;
        constexpr double kPi = 3.14159265358979323846;
        const double dx = to.longitude - from.longitude;
        const double dy = to.latitude - from.latitude;
        const double length = std::hypot(dx, dy);
        const double px = length > 0.0 ? -dy / length : 0.0;
        const double py = length > 0.0 ? dx / length : 0.0;
        for (int i = 0; i <= kSegments; ++i) {
            const double t = static_cast<double>(i) / kSegments;
            const double wobble
                = (i == 0 || i == kSegments) ? 0.0 : std::sin(t * kPi) * 0.0018;
            line.append({from.latitude + dy * t + py * wobble,
                         from.longitude + dx * t + px * wobble,
                         {}});
        }
    }

    QVector<MapStationPoint> markers;
    if (record_.hasStationLocation) {
        markers.append({to.latitude, to.longitude, record_.stationName});
    }
    // 两步写入：先折线后站点（setStations 触发最终一次重渲染）。
    routeMapPanel_->setRoutePoints(line);
    routeMapPanel_->setStations(markers);
}

} // namespace charging::client::pages::station
