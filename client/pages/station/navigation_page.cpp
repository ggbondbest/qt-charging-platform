#include "pages/station/navigation_page.h"

#include "charging/client/widgets/card.h"
#include "pages/station/platform_theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
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
QLabel#navigationStepLabel {
    color: #1F2937;
    font-size: 13px;
}
)";

void clearLayoutItems(QVBoxLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// 真实地图接入点（本轮未实现，后续迭代替换本文件内模拟逻辑即可，页面
// 结构与信号形状不变）：
//
// 1. 距离矩阵 / 路线规划（WebService API，QNetworkAccessManager 请求）：
//    用 `CHARGING_TENCENT_MAP_KEY`（环境变量注入，禁止提交进仓库）请求
//    https://apis.map.qq.com/ws/direction/v1/driving/?from=..&to=.. ，
//    解析 mode.distance / mode.duration / steps[] 替换 buildMockSteps()
//    与本文件的模拟估算；服务端域名需先在腾讯位置服务控制台绑定。
// 2. 地图渲染（本机已装 Qt6 WebEngine 时）：复用 StationMapPanel 的
//    WebEngine + tencent_map.html 通道，在本页顶部占位区渲染真实地图与
//    路线 polyline。
// 3. 距离口径统一：预约记录 distanceMeters 与推荐时段 travelMinutes
//    （ReservationService::recommendSlot）应改由上述 API 返回，两处共用
//    同一数据源。
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
        tr("导航路线为模拟数据 · 腾讯地图接口就绪后渲染真实路线（WebEngine 可用时内嵌地图）"),
        this);
    caption->setObjectName(QStringLiteral("navigationCaptionLabel"));
    caption->setWordWrap(true);
    rootLayout->addWidget(caption);

    // 长内容滚动容器：鼠标滚轮上下滚动（规格通用要求）。
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("navigationScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rootLayout->addWidget(scroll, 1);

    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 8, 0);
    contentLayout->setSpacing(10);
    scroll->setWidget(content);

    // 顶部地图占位（WebEngine + Key 就绪后替换为真实地图渲染区）。
    auto* mapPlaceholder = new QLabel(tr("🗺️ 地图路线渲染区（待腾讯地图接口接入）"), content);
    mapPlaceholder->setObjectName(QStringLiteral("navigationMapPlaceholder"));
    mapPlaceholder->setAlignment(Qt::AlignCenter);
    contentLayout->addWidget(mapPlaceholder);

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

    // 分段路线步骤列表。
    auto* stepsCard = new Card(content);
    stepsCard->setProperty("isNavigationStepsCard", true);
    auto* stepsBody = stepsCard->bodyLayout();
    auto* stepsTitle = new QLabel(tr("🧭 路线步骤"), stepsCard);
    stepsTitle->setProperty("role", QStringLiteral("sectionTitle"));
    stepsBody->addWidget(stepsTitle);
    stepsHost_ = new QWidget(stepsCard);
    stepsHost_->setObjectName(QStringLiteral("navigationStepsHost"));
    stepsLayout_ = new QVBoxLayout(stepsHost_);
    stepsLayout_->setContentsMargins(0, 0, 0, 0);
    stepsLayout_->setSpacing(6);
    stepsBody->addWidget(stepsHost_);
    contentLayout->addWidget(stepsCard);
    contentLayout->addStretch();
}

QPair<QStringList, int> NavigationPage::buildMockSteps(const ReservationRecord& record) const
{
    // 模拟分段：按行驶时长把路线切成 4~6 段，文案模板化；真实路线规划
    // API 就绪后由 steps[] 解析结果替换（见文件顶部接入点注释）。
    const auto slot = ReservationService::recommendSlot(record.distanceMeters);
    QStringList roads{
        tr("滨海大道"), tr("深南大道"), tr("科苑北路"), tr("后海滨路"),
        tr("白石路"), tr("粤海大道"),
    };
    const qint64 seed = record.reservation.chargerId + record.reservation.id;

    QStringList steps;
    steps << tr("从当前位置出发，向南驶入主路（准备出发）");
    const int segments = 3 + static_cast<int>(seed % 3); // 3~5 段行驶 + 起终
    const int totalMeters = record.distanceMeters > 0 ? record.distanceMeters : 1000;
    for (int i = 0; i < segments; ++i) {
        const int meters = totalMeters * (i + 1) / segments - totalMeters * i / segments;
        const QString road
            = roads.at(static_cast<int>((seed + i * 7) % roads.size()));
        if (i + 1 == segments) {
            steps << tr("沿 %1 行驶约 %2 米后右转进入站点").arg(road).arg(meters);
        } else if (i % 2 == 0) {
            steps << tr("沿 %1 直行约 %2 米").arg(road).arg(meters);
        } else {
            steps << tr("在路口转入 %1，继续行驶约 %2 米").arg(road).arg(meters);
        }
    }
    steps << tr("到达 %1 · 充电桩 %2，驶入场内停车位").arg(record.stationName, record.chargerCode);
    return {steps, slot.travelMinutes};
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

    const auto [steps, travelMinutes] = buildMockSteps(record_);
    QString etaText = tr("预计行驶约 %1 分钟").arg(travelMinutes);
    if (record_.startAtUtc.isValid()) {
        etaText += tr(" · 建议 %1 前出发（预约 %2 开始）")
                       .arg(record_.startAtUtc.addSecs(-(travelMinutes + 5) * 60)
                                .toLocalTime()
                                .toString(QStringLiteral("HH:mm")),
                            record_.startAtUtc.toLocalTime().toString(QStringLiteral("HH:mm")));
    }
    etaLabel_->setText(etaText);

    clearLayoutItems(stepsLayout_);
    int index = 1;
    for (const QString& step : steps) {
        auto* row = new QHBoxLayout();
        auto* badge = new QLabel(QString::number(index++), stepsHost_);
        badge->setProperty("role", QStringLiteral("secondary"));
        auto* label = new QLabel(step, stepsHost_);
        label->setObjectName(QStringLiteral("navigationStepLabel"));
        label->setWordWrap(true);
        row->addWidget(badge);
        row->addWidget(label, 1);
        stepsLayout_->addLayout(row);
    }
}

QString NavigationPage::distanceText() const
{
    return distanceLabel_->text();
}

QString NavigationPage::etaText() const
{
    return etaLabel_->text();
}

int NavigationPage::routeStepCount() const
{
    int count = 0;
    for (int i = 0; i < stepsLayout_->count(); ++i) {
        auto* item = stepsLayout_->itemAt(i);
        if (item != nullptr && item->layout() != nullptr && item->layout()->count() == 2) {
            ++count;
        }
    }
    return count;
}

} // namespace charging::client::pages::station
