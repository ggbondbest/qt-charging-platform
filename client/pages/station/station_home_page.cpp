#include "pages/station/station_home_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"
#include "pages/station/station_map_panel.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVariant>

#include <algorithm>

namespace charging::client::pages::station {

namespace {

// 页面局部样式：筛选芯片与控件（token 与全局主题一致，仅本页生效）。
const char* kStationPageStyleSheet = R"(
QWidget#stationFilterBar {
    background: #FFFFFF;
    border: 1px solid #E5E9EF;
    border-radius: 12px;
}
QPushButton[filterChip="true"] {
    background: #F4F6F8;
    border: 1px solid #D5DCE4;
    border-radius: 14px;
    padding: 6px 12px;
    font-size: 12px;
    font-weight: 600;
    color: #1F2937;
}
QPushButton[filterChip="true"]:checked {
    background: #EAF9F2;
    border: 1px solid #00B578;
    color: #00A76D;
    font-weight: 700;
}
QComboBox#priceFilterComboBox {
    background: #F4F6F8;
    border: 1px solid #D5DCE4;
    border-radius: 14px;
    padding: 5px 10px;
    font-size: 12px;
    color: #1F2937;
}
)";

QString formatPrice(qint64 centsPerKwh)
{
    return QStringLiteral("¥%1/度").arg(QString::number(centsPerKwh / 100.0, 'f', 2));
}

QString formatDistance(int meters)
{
    if (meters < 0) {
        return QStringLiteral("--");
    }
    if (meters < 1000) {
        return QStringLiteral("%1 m").arg(meters);
    }
    return QStringLiteral("%1 km").arg(meters / 1000.0, 0, 'f', 1);
}

// 清空布局中的所有子控件（重建列表卡片用）。
void clearLayoutItems(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

enum SortMode
{
    SortRecommended = 0,
    SortMostAvailable,
    SortNearest,
};

} // namespace

StationHomePage::StationHomePage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("stationHomePage"));
    setStyleSheet(QString::fromLatin1(kStationPageStyleSheet));

    service_ = new services::station::StationQueryService(this);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    // ① 地图容器区域：固定高度，不可用时面板内部降级为提示+重试，
    //    下方筛选与列表照常浏览（规格：页面仍可正常浏览）。
    mapPanel_ = new StationMapPanel(this);
    mapPanel_->setObjectName(QStringLiteral("stationMapPanel"));
    mapPanel_->setFixedHeight(170);
    rootLayout->addWidget(mapPanel_);
    // 面板内部处理“重试”（重新尝试构建地图视图）；本任务地图保持容器高度。

    // ② 筛选操作栏（地图下方、列表上方）。
    auto* filterBar = new QWidget(this);
    filterBar->setObjectName(QStringLiteral("stationFilterBar"));
    auto* filterLayout = new QHBoxLayout(filterBar);
    filterLayout->setContentsMargins(10, 6, 10, 6);
    filterLayout->setSpacing(8);

    sortGroup_ = new QButtonGroup(this);
    sortGroup_->setExclusive(true);
    sortRecommendedButton_ = new QPushButton(tr("综合"), filterBar);
    sortAvailableButton_ = new QPushButton(tr("空闲优先"), filterBar);
    sortDistanceButton_ = new QPushButton(tr("距离最近"), filterBar);
    for (auto* chip : {sortRecommendedButton_, sortAvailableButton_, sortDistanceButton_}) {
        chip->setCheckable(true);
        chip->setProperty("filterChip", true);
        chip->setCursor(Qt::PointingHandCursor);
        sortGroup_->addButton(chip);
        filterLayout->addWidget(chip);
    }
    sortRecommendedButton_->setObjectName(QStringLiteral("sortRecommendedButton"));
    sortAvailableButton_->setObjectName(QStringLiteral("sortAvailableButton"));
    sortDistanceButton_->setObjectName(QStringLiteral("sortDistanceButton"));
    sortRecommendedButton_->setChecked(true);

    auto* filterCaption = new QLabel(tr("电价"), filterBar);
    filterCaption->setProperty("role", QStringLiteral("secondary"));
    filterLayout->addSpacing(6);
    filterLayout->addWidget(filterCaption);

    priceFilterComboBox_ = new QComboBox(filterBar);
    priceFilterComboBox_->setObjectName(QStringLiteral("priceFilterComboBox"));
    priceFilterComboBox_->addItem(tr("全部电价"), -1);
    priceFilterComboBox_->addItem(tr("≤ ¥1.00"), 100);
    priceFilterComboBox_->addItem(tr("≤ ¥1.20"), 120);
    priceFilterComboBox_->addItem(tr("≤ ¥1.50"), 150);
    filterLayout->addWidget(priceFilterComboBox_);
    filterLayout->addStretch();
    rootLayout->addWidget(filterBar);

    // ③ 站点列表区域：加载 / 空 / 异常 / 列表 四态，紧跟筛选栏。
    listStack_ = new QStackedWidget(this);
    listStack_->setObjectName(QStringLiteral("stationListStack"));
    rootLayout->addWidget(listStack_, 1);

    loadingPage_ = new QWidget(listStack_);
    auto* loadingLayout = new QVBoxLayout(loadingPage_);
    auto* loadingLabel = new QLabel(tr("⏳ 正在加载附近站点…"), loadingPage_);
    loadingLabel->setObjectName(QStringLiteral("stationLoadingLabel"));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setProperty("role", QStringLiteral("secondary"));
    loadingLayout->addStretch();
    loadingLayout->addWidget(loadingLabel);
    loadingLayout->addStretch();
    listStack_->addWidget(loadingPage_);

    emptyNotice_ = new NoticePanel(QStringLiteral("🔍"), tr("没有找到相关站点"),
                                   tr("换个地址或站名关键字试试，也可以清空搜索查看全部站点。"),
                                   tr("清空搜索"), listStack_);
    connect(emptyNotice_, &NoticePanel::actionTriggered, this,
            &StationHomePage::clearKeywordAndSearch);
    listStack_->addWidget(emptyNotice_);

    errorNotice_ = new NoticePanel(QStringLiteral("⚠️"), tr("站点加载失败"), QString(),
                                   tr("重试"), listStack_);
    connect(errorNotice_, &NoticePanel::actionTriggered, this, &StationHomePage::retrySearch);
    listStack_->addWidget(errorNotice_);

    auto* scrollArea = new QScrollArea(listStack_);
    scrollArea->setObjectName(QStringLiteral("uiRecordsScroll"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    listPage_ = new QWidget(scrollArea);
    listLayout_ = new QVBoxLayout(listPage_);
    listLayout_->setContentsMargins(0, 0, 8, 0);
    listLayout_->setSpacing(10);
    scrollArea->setWidget(listPage_);
    listStack_->addWidget(scrollArea);

    connect(service_, &services::station::StationQueryService::queryStarted, this,
            &StationHomePage::handleQueryStarted);
    connect(service_, &services::station::StationQueryService::querySucceeded, this,
            &StationHomePage::handleQuerySucceeded);
    connect(service_, &services::station::StationQueryService::queryFailed, this,
            &StationHomePage::handleQueryFailed);

    // 筛选变更 → 即时刷新列表（本地投影，不重新请求）。
    connect(sortGroup_, &QButtonGroup::idClicked, this,
            [this](int) { refreshFilteredCards(); });
    connect(priceFilterComboBox_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshFilteredCards(); });

    // 进入页面即拉取一次“附近站点”。
    setViewState(ViewState::Loading);
    search(QString());
}

void StationHomePage::search(const QString& keyword)
{
    keyword_ = keyword.trimmed();
    service_->search(keyword_);
}

services::station::StationQueryService* StationHomePage::service() const
{
    return service_;
}

StationMapPanel* StationHomePage::mapPanel() const
{
    return mapPanel_;
}

StationHomePage::ViewState StationHomePage::viewState() const
{
    return viewState_;
}

QString StationHomePage::currentKeyword() const
{
    return keyword_;
}

void StationHomePage::setViewState(ViewState state)
{
    viewState_ = state;
    switch (state) {
    case ViewState::Loading:
        listStack_->setCurrentWidget(loadingPage_);
        break;
    case ViewState::Empty:
        listStack_->setCurrentWidget(emptyNotice_);
        break;
    case ViewState::Error:
        listStack_->setCurrentWidget(errorNotice_);
        break;
    case ViewState::List:
        listStack_->setCurrentIndex(3);
        break;
    }
}

void StationHomePage::handleQueryStarted()
{
    setViewState(ViewState::Loading);
}

void StationHomePage::handleQuerySucceeded(
    const services::station::StationList& stations)
{
    lastResults_ = stations;

    QVector<MapStationPoint> mapPoints;
    mapPoints.reserve(stations.size());
    for (const auto& item : stations) {
        mapPoints.append({item.station.latitude, item.station.longitude, item.station.name});
    }
    mapPanel_->setStations(mapPoints);

    refreshFilteredCards();
}

void StationHomePage::handleQueryFailed(const QString& message)
{
    // 异常状态：友好提示 + 重试；不展示原始错误码。
    errorNotice_->setContent(QStringLiteral("⚠️"), tr("站点加载失败"), message, tr("重试"));
    setViewState(ViewState::Error);
}

void StationHomePage::refreshFilteredCards()
{
    const int maxPriceCents = priceFilterComboBox_->currentData().toInt();
    int sortMode = SortRecommended;
    if (sortAvailableButton_->isChecked()) {
        sortMode = SortMostAvailable;
    } else if (sortDistanceButton_->isChecked()) {
        sortMode = SortNearest;
    }

    services::station::StationList filtered;
    for (const auto& item : lastResults_) {
        if (maxPriceCents > 0 && item.station.priceCentsPerKwh > maxPriceCents) {
            continue;
        }
        filtered.append(item);
    }
    std::stable_sort(filtered.begin(), filtered.end(), [sortMode](const auto& left,
                                                                  const auto& right) {
        switch (sortMode) {
        case SortMostAvailable:
            if (left.station.availableChargers != right.station.availableChargers) {
                return left.station.availableChargers > right.station.availableChargers;
            }
            break;
        case SortNearest: {
            const auto leftDistance = left.distanceMeters < 0 ? 1 << 30 : left.distanceMeters;
            const auto rightDistance = right.distanceMeters < 0 ? 1 << 30 : right.distanceMeters;
            return leftDistance < rightDistance;
        }
        case SortRecommended:
        default:
            break;
        }
        return false; // 综合：保持服务端返回顺序。
    });

    clearLayoutItems(listLayout_);
    for (const auto& item : filtered) {
        listLayout_->addWidget(createStationCard(item));
    }
    listLayout_->addStretch();

    if (filtered.isEmpty()) {
        setViewState(ViewState::Empty);
    } else {
        setViewState(ViewState::List);
    }
}

void StationHomePage::retrySearch()
{
    search(keyword_);
}

void StationHomePage::clearKeywordAndSearch()
{
    search(QString());
}

QWidget* StationHomePage::createStationCard(const services::station::StationListItem& item)
{
    auto* card = new ClickableCard(listPage_);
    // 保留 Card 的 uiCard objectName（全局 QSS 依赖），用动态属性标记业务身份。
    card->setProperty("isStationCard", true);
    card->setProperty("stationId", item.station.id);
    card->setCursor(Qt::PointingHandCursor);

    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    auto* nameLabel = new QLabel(item.station.name, card);
    nameLabel->setProperty("role", QStringLiteral("sectionTitle"));
    auto* availabilityTag = new StatusTag(
        item.station.availableChargers > 0
            ? tr("空闲 %1/%2").arg(item.station.availableChargers).arg(item.station.totalChargers)
            : tr("桩位已满"),
        item.station.availableChargers > 0 ? StatusTag::Tone::Success : StatusTag::Tone::Danger,
        card);
    titleRow->addWidget(nameLabel);
    titleRow->addStretch();
    titleRow->addWidget(availabilityTag);
    body->addLayout(titleRow);

    auto* addressLabel = new QLabel(item.station.address, card);
    addressLabel->setProperty("role", QStringLiteral("secondary"));
    body->addWidget(addressLabel);

    auto* detailRow = new QHBoxLayout();
    auto* priceLabel = new QLabel(formatPrice(item.station.priceCentsPerKwh), card);
    priceLabel->setProperty("role", QStringLiteral("amountStrong"));
    auto* distanceLabel = new QLabel(tr("距离 %1").arg(formatDistance(item.distanceMeters)),
                                     card);
    distanceLabel->setProperty("role", QStringLiteral("secondary"));
    detailRow->addWidget(priceLabel);
    detailRow->addStretch();
    detailRow->addWidget(distanceLabel);
    body->addLayout(detailRow);

    const charging::model::Station station = item.station;
    const int distanceMeters = item.distanceMeters;
    connect(card, &ClickableCard::clicked, this,
            [this, station, distanceMeters]() { emit stationSelected(station, distanceMeters); });

    return card;
}

int StationHomePage::stationCardCount() const
{
    return visibleStationIds().size();
}

QVector<qint64> StationHomePage::visibleStationIds() const
{
    // 以布局为准（deleteLater 的旧卡片已脱离布局，不会被计入）。
    QVector<qint64> ids;
    for (int i = 0; i < listLayout_->count(); ++i) {
        const auto* item = listLayout_->itemAt(i);
        if (item == nullptr || item->widget() == nullptr) {
            continue;
        }
        if (item->widget()->property("isStationCard").toBool()) {
            ids.append(item->widget()->property("stationId").toLongLong());
        }
    }
    return ids;
}

QWidget* StationHomePage::stationCardAt(int index) const
{
    int seen = 0;
    for (int i = 0; i < listLayout_->count(); ++i) {
        const auto* item = listLayout_->itemAt(i);
        if (item == nullptr || item->widget() == nullptr) {
            continue;
        }
        if (item->widget()->property("isStationCard").toBool()) {
            if (seen == index) {
                return item->widget();
            }
            ++seen;
        }
    }
    return nullptr;
}

} // namespace charging::client::pages::station
