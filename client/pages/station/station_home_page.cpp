#include "pages/station/station_home_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"
#include "pages/station/station_filter_dialog.h"
#include "pages/station/station_map_panel.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
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
QPushButton[isStationStar="true"] {
    background: transparent;
    border: none;
    font-size: 18px;
    padding: 2px 6px;
    color: #9CA3AF;
}
QPushButton[isStationStar="true"]:hover {
    color: #00B578;
}
QPushButton[isStationStar="true"][starred="true"] {
    color: #00B578;
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

    // ① 地图 + 列表可拖拽分屏（成员 2 地图迭代）：上半为地图面板、下半为
    //    “筛选栏+列表”容器，分隔条可上下拖动——把地图拉大或把列表压小。
    //    迭代 3：初始尺寸改走 attachToSplitter 统一助手（降级 56 / 真地图
    //    kPreferredInitialHeight=420，异步加载成功后升档），与导航页地图
    //    同口径——修复构造期 isDegraded 恒真导致的“初始小地图”观感；
    //    面板内部“降级/重试/标记点”逻辑零改动（规格：页面仍可正常浏览）。
    mapPanel_ = new StationMapPanel(this);
    mapPanel_->setObjectName(QStringLiteral("stationMapPanel"));
    mapPanel_->setMinimumHeight(56); // 压到最低仍保降级横幅一行可读
    auto* mapListSplitter = new QSplitter(Qt::Vertical, this);
    mapListSplitter->setObjectName(QStringLiteral("stationMapListSplitter"));
    mapListSplitter->setHandleWidth(10);
    mapListSplitter->addWidget(mapPanel_);
    auto* listPane = new QWidget(mapListSplitter);
    listPane->setObjectName(QStringLiteral("stationListPane"));
    auto* listPaneLayout = new QVBoxLayout(listPane);
    listPaneLayout->setContentsMargins(0, 0, 0, 0);
    listPaneLayout->setSpacing(rootLayout->spacing());
    mapListSplitter->addWidget(listPane);
    mapListSplitter->setStretchFactor(0, 0); // 拉伸增量归列表，地图保持默认高
    mapListSplitter->setStretchFactor(1, 1);
    mapPanel_->attachToSplitter(mapListSplitter, 600);
    rootLayout->addWidget(mapListSplitter, 1);
    // 面板内部处理“重试”（重新尝试构建地图视图）；分屏高度由用户拖动决定。

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
    listPaneLayout->addWidget(filterBar);

    // ③ 站点列表区域：加载 / 空 / 异常 / 列表 四态，紧跟筛选栏。
    listStack_ = new QStackedWidget(this);
    listStack_->setObjectName(QStringLiteral("stationListStack"));
    listPaneLayout->addWidget(listStack_, 1);

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
            &StationHomePage::handleEmptyAction);
    listStack_->addWidget(emptyNotice_);

    errorNotice_ = new NoticePanel(QStringLiteral("⚠️"), tr("站点加载失败"), QString(),
                                   tr("重试"), listStack_);
    connect(errorNotice_, &NoticePanel::actionTriggered, this, &StationHomePage::retrySearch);
    listStack_->addWidget(errorNotice_);

    auto* scrollArea = new QScrollArea(listStack_);
    scrollArea->setObjectName(QStringLiteral("uiRecordsScroll"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    // 卡片内容自适应宽度（配合下方标签换行），杜绝多余的横向滚动条。
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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

    // 迭代 3：收藏态变化 → 星星回显（服务实例懒建，见 favoritesService()）。
    connect(favoritesService(), &services::favorites::FavoritesService::favoritesChanged, this,
            &StationHomePage::refreshStarButtons);

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
    // 地图标记刷新统一收敛到 refreshFilteredCards（与列表筛选同口径）。
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

    // 迭代 3：高级筛选先过一遍（服务层纯客户端投影，组内 OR / 组间 AND），
    // 再叠加既有的电价筛选与排序——全部投影于 lastResults_，不重发请求。
    services::station::StationList filtered
        = services::station::applyStationFilter(lastResults_, filterCriteria_);
    services::station::StationList visible;
    for (const auto& item : filtered) {
        if (maxPriceCents > 0 && item.station.priceCentsPerKwh > maxPriceCents) {
            continue;
        }
        visible.append(item);
    }
    std::stable_sort(visible.begin(), visible.end(), [sortMode](const auto& left,
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
    for (const auto& item : visible) {
        listLayout_->addWidget(createStationCard(item));
    }
    listLayout_->addStretch();

    // 地图标记与列表同口径（筛选后所见即所得）。
    QVector<MapStationPoint> mapPoints;
    mapPoints.reserve(visible.size());
    for (const auto& item : visible) {
        mapPoints.append({item.station.latitude, item.station.longitude, item.station.name});
    }
    mapPanel_->setStations(mapPoints);

    if (visible.isEmpty()) {
        // 空态文案分口径：有筛选条件在生效 → “暂无符合条件的充电站”（迭代 3
        // 规格）；纯关键字无结果 → 原有“换个关键词”引导。
        const bool criteriaActive = !filterCriteria_.isEmpty() || maxPriceCents > 0;
        emptyNotice_->setContent(
            QStringLiteral("🔍"),
            criteriaActive ? tr("暂无符合条件的充电站") : tr("没有找到相关站点"),
            criteriaActive ? tr("当前筛选条件下没有电站，试试放宽或重置筛选条件。")
                           : tr("换个地址或站名关键字试试，也可以清空搜索查看全部站点。"),
            criteriaActive ? tr("重置筛选") : tr("清空搜索"));
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
    // 长站名/地址不换行会撑宽滚动区内容（右侧空桩标签被裁 + 横向滚动条）。
    nameLabel->setWordWrap(true);
    auto* availabilityTag = new StatusTag(
        item.station.availableChargers > 0
            ? tr("空闲 %1/%2").arg(item.station.availableChargers).arg(item.station.totalChargers)
            // 满桩是"占用"（warning）不是"故障"（danger）——状态色 token
            // 语义：空闲绿 / 占用黄 / 故障红 / 离线灰（见 client_platform.qss）。
            : tr("桩位已满"),
        item.station.availableChargers > 0 ? StatusTag::Tone::Success : StatusTag::Tone::Warning,
        card);
    titleRow->addWidget(nameLabel);
    titleRow->addStretch();
    titleRow->addWidget(availabilityTag);
    body->addLayout(titleRow);

    auto* addressLabel = new QLabel(item.station.address, card);
    addressLabel->setProperty("role", QStringLiteral("secondary"));
    addressLabel->setWordWrap(true);
    body->addWidget(addressLabel);

    auto* detailRow = new QHBoxLayout();
    auto* priceLabel = new QLabel(formatPrice(item.station.priceCentsPerKwh), card);
    priceLabel->setProperty("role", QStringLiteral("amountStrong"));
    auto* distanceLabel = new QLabel(tr("距离 %1").arg(formatDistance(item.distanceMeters)),
                                     card);
    distanceLabel->setProperty("role", QStringLiteral("secondary"));

    // 迭代 3：右下角收藏星星。必须是真 QPushButton——ClickableCard 对
    // 区域内任意左键释放都发 clicked()，只有按钮自身消费事件才能防误触详情。
    const qint64 stationId = item.station.id;
    auto* starButton = new QPushButton(card);
    starButton->setObjectName(QStringLiteral("stationCardStar_%1").arg(stationId));
    starButton->setProperty("isStationStar", true);
    starButton->setCursor(Qt::PointingHandCursor);
    starButton->setToolTip(tr("收藏该站点"));
    connect(starButton, &QPushButton::clicked, this,
            [this, stationId]() { favoritesService()->toggle(stationId); });

    detailRow->addWidget(priceLabel);
    detailRow->addStretch();
    detailRow->addWidget(distanceLabel);
    detailRow->addWidget(starButton);
    body->addLayout(detailRow);

    applyStarState(starButton, stationId);

    const charging::model::Station station = item.station;
    const int distanceMeters = item.distanceMeters;
    connect(card, &ClickableCard::clicked, this,
            [this, station, distanceMeters]() { emit stationSelected(station, distanceMeters); });

    return card;
}

// 星星回显：空心 ☆（未收藏，灰）↔ 实心 ★（已收藏，绿高亮，QSS starred 属性）。
void StationHomePage::applyStarState(QPushButton* starButton, qint64 stationId) const
{
    const bool starred = favoritesService_ != nullptr && favoritesService_->contains(stationId);
    starButton->setText(starred ? QStringLiteral("★") : QStringLiteral("☆"));
    starButton->setProperty("starred", starred);
    // 属性选择器变化必须重抛光（QStyleSheetStyle 只在 polish 时求值属性规则，
    // 仓内既有口径：status_tag/login_page 等成对 unpolish+polish）——否则已
    // 显示卡片的高亮色不随收藏状态更新（验收缺陷 3）。
    starButton->style()->unpolish(starButton);
    starButton->style()->polish(starButton);
    starButton->setAccessibleName(starred ? tr("取消收藏") : tr("收藏"));
    starButton->setToolTip(starred ? tr("取消收藏") : tr("收藏该站点"));
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

// —— 迭代 3 · 收藏 + 高级筛选 ——

void StationHomePage::setFavoritesService(services::favorites::FavoritesService* service)
{
    if (service == nullptr || service == favoritesService_) {
        return;
    }
    if (favoritesService_ != nullptr) {
        disconnect(favoritesService_, nullptr, this, nullptr);
    }
    favoritesService_ = service;
    connect(favoritesService_, &services::favorites::FavoritesService::favoritesChanged, this,
            &StationHomePage::refreshStarButtons);
    refreshStarButtons(); // 换号/注入后回显该用户收藏
}

services::favorites::FavoritesService* StationHomePage::favoritesService()
{
    if (favoritesService_ == nullptr) {
        // 未注入兜底：页面自建（父对象随页面析构；未登录 = 内存态口径）。
        favoritesService_ = new services::favorites::FavoritesService(this);
        connect(favoritesService_, &services::favorites::FavoritesService::favoritesChanged, this,
                &StationHomePage::refreshStarButtons);
    }
    return favoritesService_;
}

void StationHomePage::refreshStarButtons()
{
    // 星星挂在卡片内（孙级），全量找后按属性过滤。
    for (QPushButton* button : findChildren<QPushButton*>()) {
        if (!button->property("isStationStar").toBool()) {
            continue;
        }
        applyStarState(button, button->parentWidget()->property("stationId").toLongLong());
    }
}

void StationHomePage::handleEmptyAction()
{
    // 与空态文案同口径（验收缺陷 2）：criteriaActive = 高级筛选 OR 电价筛选，
    // 只要任一生效就重置全部筛选条件、保留用户关键词；纯关键词空态才清词重搜。
    const bool priceActive = priceFilterComboBox_->currentData().toInt() > 0;
    if (filterCriteria_.isEmpty() && !priceActive) {
        clearKeywordAndSearch();
        return;
    }
    if (priceActive) {
        QSignalBlocker blocker(priceFilterComboBox_); // 防逐信号重复投影
        priceFilterComboBox_->setCurrentIndex(0);
    }
    if (!filterCriteria_.isEmpty()) {
        setFilterCriteria(services::station::StationFilterCriteria()); // 内部含投影刷新
    } else {
        refreshFilteredCards();
    }
}

void StationHomePage::openFilterDialog()
{
    if (filterDialog_ != nullptr) {
        filterDialog_->raise();
        filterDialog_->activateWindow();
        return;
    }
    auto* dialog = new StationFilterDialog(filterCriteria_, this);
    connect(dialog, &StationFilterDialog::applied, this,
            &StationHomePage::setFilterCriteria);
    filterDialog_ = dialog;
    dialog->show(); // 非模态（设置页弹窗口径）
}

void StationHomePage::setFilterCriteria(const services::station::StationFilterCriteria& criteria)
{
    filterCriteria_ = criteria;
    refreshFilteredCards(); // 纯投影刷新，不重发请求
}

services::station::StationFilterCriteria StationHomePage::filterCriteria() const
{
    return filterCriteria_;
}

} // namespace charging::client::pages::station
