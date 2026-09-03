#include "pages/station/station_map_panel.h"

#include "charging/client/widgets/notice_panel.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVBoxLayout>
#include <QtGlobal>

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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    degradedNotice_ = new NoticePanel(QStringLiteral("🗺"), tr("地图暂不可用"), QString(),
                                      tr("重试"), this);
    connect(degradedNotice_, &NoticePanel::actionTriggered, this, [this]() {
        tryBuildMapView();
        emit retryRequested();
    });
    layout->addWidget(degradedNotice_);

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
    degradedNotice_->setContent(QStringLiteral("🗺"), title, description, tr("重试"));
    degradedNotice_->show();
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
        degradedNotice_->hide();
    });
    static_cast<QVBoxLayout*>(layout())->addWidget(view);
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

    html.replace(QStringLiteral("%TENCENT_MAP_KEY%"), mapKey());
    html.replace(QStringLiteral("%CENTER_LAT%"), formatCoordinate(kDefaultCenterLatitude));
    html.replace(QStringLiteral("%CENTER_LNG%"), formatCoordinate(kDefaultCenterLongitude));
    html.replace(QStringLiteral("%STATION_POINTS%"),
                 QString::fromUtf8(QJsonDocument(points).toJson(QJsonDocument::Compact)));
    return html;
}

} // namespace charging::client::pages::station
