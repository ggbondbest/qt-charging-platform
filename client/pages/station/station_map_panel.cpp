#include "pages/station/station_map_panel.h"

#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QtGlobal>
#include <QVBoxLayout>

#ifdef CHARGING_PLATFORM_HAS_WEBENGINE
#include <QWebEngineView>
#endif

// AUTORCC 生成的资源初始化函数位于全局命名空间，Q_INIT_RESOURCE 必须在全局作用域调用。
static void ensureStationResourceRegistered()
{
    Q_INIT_RESOURCE(station_resources);
}

namespace charging::client::pages::station {

namespace {

// 默认展示中心（深圳南山科技园，演示坐标）；真实位置由站点查询接口提供。
constexpr double kDefaultCenterLatitude = 22.541;
constexpr double kDefaultCenterLongitude = 113.943;

QString formatCoordinate(double value)
{
    return QString::number(value, 'f', 6);
}

} // namespace

StationMapPanel::StationMapPanel(QWidget* parent) : QWidget(parent)
{
    // 本模块 qrc（地图页面模板）需要显式初始化。
    ensureStationResourceRegistered();

    // 局部样式（token 与全局主题一致，仅本面板生效）。
    setStyleSheet(QString::fromLatin1(R"(
QFrame#mapDegradedBanner {
    background: #FFF7E8;
    border: 1px solid #F0B860;
    border-radius: 12px;
}
QLabel#mapDegradedLabel {
    color: #8A5A00;
    font-size: 12px;
    font-weight: 600;
    background: transparent;
    border: none;
}
QPushButton#mapRetryButton {
    background: #FFFFFF;
    color: #8A5A00;
    border: 1px solid #E0B667;
    border-radius: 13px;
    padding: 4px 12px;
    font-size: 12px;
    font-weight: 600;
}
)"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);

    // 一行式降级横幅：告知"地图不可用"但不占用整块地图高度。
    degradedBanner_ = new QFrame(this);
    degradedBanner_->setObjectName(QStringLiteral("mapDegradedBanner"));
    auto* bannerLayout = new QHBoxLayout(degradedBanner_);
    bannerLayout->setContentsMargins(12, 7, 10, 7);
    bannerLayout->setSpacing(8);
    degradedBannerLabel_ = new QLabel(degradedBanner_);
    degradedBannerLabel_->setObjectName(QStringLiteral("mapDegradedLabel"));
    degradedBannerLabel_->setWordWrap(true);
    auto* retryButton = new QPushButton(tr("重试"), degradedBanner_);
    retryButton->setObjectName(QStringLiteral("mapRetryButton"));
    retryButton->setCursor(Qt::PointingHandCursor);
    connect(retryButton, &QPushButton::clicked, this, [this]() {
        tryBuildMapView();
        emit retryRequested();
    });
    bannerLayout->addWidget(degradedBannerLabel_, 1);
    bannerLayout->addWidget(retryButton, 0, Qt::AlignVCenter);
    degradedBanner_->setVisible(false);
    layout->addWidget(degradedBanner_);
    layout->addStretch();

    tryBuildMapView();
}

bool StationMapPanel::isDegraded() const
{
    return degraded_;
}

void StationMapPanel::setStations(const QVector<MapStationPoint>& stations)
{
    stations_ = stations;
#ifdef CHARGING_PLATFORM_HAS_WEBENGINE
    if (!degraded_ && mapView_ != nullptr) {
        auto* view = static_cast<QWebEngineView*>(mapView_);
        view->setHtml(buildMapHtml(), QUrl(QStringLiteral("https://map.qq.com/")));
    }
#endif
}

void StationMapPanel::setRoutePoints(const QVector<MapStationPoint>& points)
{
    routePoints_ = points;
    // 刷新通道与 setStations 完全一致（数据全量重注入 html），直接复用；
    // 降级/未构建时只缓存，重试成功后随 setStations 一并渲染。
    setStations(stations_);
}

QString StationMapPanel::mapKey()
{
    return qEnvironmentVariable("CHARGING_TENCENT_MAP_KEY").trimmed();
}

void StationMapPanel::showDegraded(const QString& title, const QString& description)
{
    degraded_ = true;
    if (mapView_ != nullptr) {
        if (auto* panelLayout = static_cast<QVBoxLayout*>(layout())) {
            panelLayout->removeWidget(mapView_);
        }
        mapView_->deleteLater();
        mapView_ = nullptr;
    }
    // 完整原因文案收进 tooltip，横幅本身保持一行。
    degradedBannerLabel_->setText(tr("%1 · 不影响下方电站列表与预约").arg(title));
    degradedBannerLabel_->setToolTip(description);
    degradedBanner_->setVisible(true);
}

void StationMapPanel::tryBuildMapView()
{
    const QString key = mapKey();
    if (key.isEmpty()) {
        showDegraded(tr("未配置地图服务"),
                     // 规格要求：不得向用户展示原始环境变量字符串。
                     tr("暂未检测到地图服务配置，站点列表不受影响；"
                        "完成配置后点击“重试”即可加载地图。"));
        return;
    }

#ifdef CHARGING_PLATFORM_HAS_WEBENGINE
    if (mapView_ != nullptr) {
        return;
    }
    auto* view = new QWebEngineView(this);
    connect(view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) {
            showDegraded(tr("地图加载失败"),
                         tr("腾讯地图页面加载失败，请检查网络后点击“重试”。"));
            return;
        }
        degraded_ = false;
        degradedBanner_->setVisible(false);
    });
    auto* panelLayout = static_cast<QVBoxLayout*>(layout());
    // 功能修正：构造尾部有一根占位 stretch（降级横幅态撑位用），若不摘除，
    // 地图会被追加到 stretch 之后、永远停在 sizeHint 高度，外层分栏拖动只
    // 放大空白。摘掉后地图自身吃掉面板全部剩余高度（横幅在最前，不受影响）。
    if (QLayoutItem* trailing = panelLayout->takeAt(panelLayout->count() - 1)) {
        if (trailing->spacerItem() != nullptr) {
            delete trailing;
        } else {
            panelLayout->addItem(trailing);
        }
    }
    panelLayout->addWidget(view);
    mapView_ = view;
    view->setHtml(buildMapHtml(), QUrl(QStringLiteral("https://map.qq.com/")));
#else
    showDegraded(tr("地图组件不可用"),
                 tr("当前运行环境未安装 Qt WebEngine 模块，地图入口已降级；"
                    "安装后可展示腾讯地图，不影响电站列表与预约。"));
#endif
}

QString StationMapPanel::buildMapHtml() const
{
    QFile templateFile(QStringLiteral(":/station/tencent_map.html"));
    QString html;
    if (templateFile.open(QIODevice::ReadOnly)) {
        html = QString::fromUtf8(templateFile.readAll());
    } else {
        return html;
    }

    QJsonArray points;
    for (const auto& station : stations_) {
        QJsonArray point;
        point.append(station.latitude);
        point.append(station.longitude);
        point.append(station.name);
        points.append(point);
    }

    QJsonArray route;
    for (const auto& point : routePoints_) {
        QJsonArray pair;
        pair.append(point.latitude);
        pair.append(point.longitude);
        route.append(pair);
    }

    html.replace(QStringLiteral("%TENCENT_MAP_KEY%"), mapKey());
    html.replace(QStringLiteral("%CENTER_LAT%"), formatCoordinate(kDefaultCenterLatitude));
    html.replace(QStringLiteral("%CENTER_LNG%"), formatCoordinate(kDefaultCenterLongitude));
    html.replace(QStringLiteral("%STATION_POINTS%"),
                 QString::fromUtf8(QJsonDocument(points).toJson(QJsonDocument::Compact)));
    html.replace(QStringLiteral("%ROUTE_POINTS%"),
                 QString::fromUtf8(QJsonDocument(route).toJson(QJsonDocument::Compact)));
    return html;
}

} // namespace charging::client::pages::station
