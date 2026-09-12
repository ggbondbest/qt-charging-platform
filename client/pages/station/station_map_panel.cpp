// 文件职责：StationMapPanel 实现（成员 2，需求 #22）——腾讯地图 WebEngine
// HTML 壳与一行式降级横幅的双通道渲染面板。
// 被谁用：StationHomePage（找站分栏上半区）与 NavigationPage（路线地图），
// 经 attachToSplitter 统一分栏口径；测试 tst_station_map_panel 直接构造。
// 数据流向：页面 → setStations/setRoutePoints（先缓存、可用时才渲染）→
// buildMapHtml 填 qrc 模板 :/station/tencent_map.html 的占位符 →
// QWebEngineView::setHtml 注入内存页；Key 运行时读环境变量，全程不触 TCP 契约。
// 构建宏 CHARGING_PLATFORM_HAS_WEBENGINE 决定真图通道是否存在（无则恒降级）。
#include "pages/station/station_map_panel.h"

#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QtGlobal>
#include <QVBoxLayout>

#include <memory>

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

// 中心点占位符专用：定点 6 位小数（约 0.1m 精度）输出字面量；站点/路线
// 点位则走 JSON 序列化直出 double——两条注入通道格式不同，模板各按己所需消费。
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
        // 一次点击两件事：本地先重建视图（配好 Key 即直接换真图，无需外层
        // 参与）；retryRequested 是留给外层的契约信号（由外层决定是否另有
        // 页面级补救），当前仓库内暂无接线方。
        tryBuildMapView();
        emit retryRequested();
    });
    bannerLayout->addWidget(degradedBannerLabel_, 1);
    bannerLayout->addWidget(retryButton, 0, Qt::AlignVCenter);
    degradedBanner_->setVisible(false);
    layout->addWidget(degradedBanner_);
    layout->addStretch();

    // 构造尾部即尝试建图：面板进入页面时要么已在加载真图、要么已挂出降级
    // 横幅，不留“未决”空窗；异步 loadFinished 成功只是进一步 mapReady 升档
    // （degraded_ 初值 true，见 attachToSplitter 起步口径）。
    tryBuildMapView();
}

bool StationMapPanel::isDegraded() const
{
    return degraded_;
}

// 刷新口径 = 整页重注入：无增量 JS 协议，数据一变即 setHtml 重建内存页
// （站点量小，代价可接受）；降级/无 WebEngine 构建时本方法退化为纯缓存写。
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

void StationMapPanel::attachToSplitter(QSplitter* splitter, int listPaneInitial)
{
    // 构造期地图必然还在异步加载（degraded_ 初值 true），直接按降级口径
    // 起步；mapReady 到达后再升档，修复"初始小地图"观感（用户反馈）。
    constexpr int kDegradedSplitHeight = 56; // 一行降级横幅的贴合高度
    splitter->setSizes({degraded_ ? kDegradedSplitHeight : kPreferredInitialHeight,
                        listPaneInitial});
    // 手动拖动标记：shared_ptr 让 splitterMoved/mapReady 两个 lambda 共享
    // 同一份状态；用户一旦拖过分栏，后续 mapReady 升档永久作废。
    auto dragged = std::make_shared<bool>(false);
    connect(splitter, &QSplitter::splitterMoved, splitter,
            [dragged]() { *dragged = true; });
    connect(this, &StationMapPanel::mapReady, this,
            [this, splitter, dragged, kDegradedSplitHeight]() {
                if (*dragged) {
                    return; // 用户手动调过分栏，尊重其选择
                }
                const QList<int> sizes = splitter->sizes();
                if (sizes.size() != 2) {
                    return;
                }
                // 升档增量从列表半区等量扣除（保底一行横幅高），分栏总高
                // 不变——地图变大不是挤压窗口，而是从列表让渡。
                const int grow = kPreferredInitialHeight - sizes.at(0);
                if (grow > 0) {
                    splitter->setSizes({kPreferredInitialHeight,
                                        qMax(kDegradedSplitHeight, sizes.at(1) - grow)});
                }
            });
}

QString StationMapPanel::mapKey()
{
    return qEnvironmentVariable("CHARGING_TENCENT_MAP_KEY").trimmed();
}

// 降级统一落点（三条出口共用）：置降级位 → 移除并销毁已建视图（若有）→
// 亮横幅。横幅构造期常驻、只做显隐切换，这里不重建它。
void StationMapPanel::showDegraded(const QString& title, const QString& description)
{
    degraded_ = true;
    if (mapView_ != nullptr) {
        if (auto* panelLayout = static_cast<QVBoxLayout*>(layout())) {
            panelLayout->removeWidget(mapView_);
        }
        // deleteLater 而非 delete：调用链可能正处在视图自身 loadFinished 的
        // 信号栈上，同步析构会在返回途中踩到已销毁对象。
        mapView_->deleteLater();
        mapView_ = nullptr;
    }
    // 完整原因文案收进 tooltip，横幅本身保持一行。
    degradedBannerLabel_->setText(tr("%1 · 不影响下方电站列表与预约").arg(title));
    degradedBannerLabel_->setToolTip(description);
    degradedBanner_->setVisible(true);
}

// 建图/降级的唯一总闸（构造与“重试”共用）：判定顺序 = 环境 Key 在前、
// 构建宏与实例复用其次、加载结果兜底；三条降级出口全部汇到 showDegraded。
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
    // 幂等复用闸：已有视图（在途加载或已建成）时直接返回——重试不会造出
    // 第二个 WebEngine 实例，也不会重发 setHtml 打断在途加载。
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
        if (!mapReadyEmitted_) {
            // setStations 每次重渲染都会回到这里，升档信号只发首次。
            mapReadyEmitted_ = true;
            emit mapReady();
        }
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

// 地图 HTML 装配单点：qrc 模板读不到直接返回空 HTML（setHtml 收到空串即
// 空白页）；否则把缓存的站点/路线点位序列化成紧凑 JSON，连同环境 Key 与
// 默认中心点一起替换进模板的 5 个 %占位符%。
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
