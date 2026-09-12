// FavoritesPage 实现（成员 2，迭代 3 批）。数据流：收藏 ID 来自 HomeShell 注入的
// FavoritesService（QSettings 按用户持久化），站点资料来自本页自建模拟查询
// 通道；「收藏 ∩ 查询 → 筛选投影」的拼接全部发生在 refresh()。
// QML 孪生页口径对照见 docs/design/qml-station-mapping.md。
#include "pages/station/favorites_page.h"

#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"
#include "pages/station/station_filter_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

using services::favorites::FavoritesService;
using services::station::StationFilterCriteria;
using services::station::StationListItem;
using services::station::StationList;

// 页面局部样式：token 与找站页同口径（同值不复用页面级串——页面级样式不跨页
// 生效），仅本页使用。星星样式与首页一致（实心绿高亮）。
const char* kFavoritesPageStyleSheet = R"(
QWidget#favoritesPage {
    background: #F7F9FB;
}
QLabel#favoritesPageTitle {
    color: #1F2937;
    font-size: 16px;
    font-weight: 700;
}
QPushButton#favoritesFilterButton {
    background: #F4F6F8;
    border: 1px solid #D5DCE4;
    border-radius: 14px;
    padding: 6px 12px;
    font-size: 12px;
    font-weight: 600;
    color: #1F2937;
}
QPushButton#favoritesFilterButton[filterActive="true"] {
    background: #EAF9F2;
    border: 1px solid #00B578;
    color: #00A76D;
}
QPushButton[isStationStar="true"] {
    background: transparent;
    border: none;
    font-size: 18px;
    padding: 2px 6px;
    color: #00B578;
}
QLabel#favoritesEmptyHint {
    color: #9CA3AF;
    font-size: 12px;
}
)";

// 展示助手与 station_detail_page 同名同值（匿名 namespace 页私有，不共享
// 头文件——为两行格式化引入跨页耦合不值，口径一致性由测试钉住）。
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

FavoritesPage::FavoritesPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("favoritesPage"));
    setStyleSheet(QString::fromLatin1(kFavoritesPageStyleSheet));

    // 本页自建全量查询通道：独立实例 = 独立信号——若复用找站页的共享实例，
    // 其关键词 search() 同样会打进本页 querySucceeded 槽，把搜索中间结果
    // 污染成收藏候选池。
    service_ = new services::station::StationQueryService(this);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    // 标题行：收藏标题 + 高级筛选入口（复用 StationFilterDialog 组件）。
    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("我的收藏"), this);
    titleLabel->setObjectName(QStringLiteral("favoritesPageTitle"));
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    auto* filterButton = new QPushButton(tr("▽ 高级筛选"), this);
    filterButton->setObjectName(QStringLiteral("favoritesFilterButton"));
    filterButton->setCursor(Qt::PointingHandCursor);
    connect(filterButton, &QPushButton::clicked, this, &FavoritesPage::openFilterDialog);
    headerRow->addWidget(filterButton);
    rootLayout->addLayout(headerRow);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("favoritesStack"));
    rootLayout->addWidget(stack_, 1);

    loadingPage_ = new QWidget(stack_);
    auto* loadingLayout = new QVBoxLayout(loadingPage_);
    auto* loadingLabel = new QLabel(tr("⏳ 正在加载收藏站点…"), loadingPage_);
    loadingLabel->setObjectName(QStringLiteral("favoritesLoadingLabel"));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setProperty("role", QStringLiteral("secondary"));
    loadingLayout->addStretch();
    loadingLayout->addWidget(loadingLabel);
    loadingLayout->addStretch();
    stack_->addWidget(loadingPage_);

    emptyNotice_ = new NoticePanel(QStringLiteral("⭐"), tr("暂无收藏的充电站"),
                                   tr("在找站页点击卡片右下角 ☆ 即可收藏，收藏后在这里集中管理。"),
                                   QString(), stack_);
    stack_->addWidget(emptyNotice_);

    errorNotice_ = new NoticePanel(QStringLiteral("⚠️"), tr("收藏列表加载失败"), QString(),
                                   tr("重试"), stack_);
    connect(errorNotice_, &NoticePanel::actionTriggered, this, &FavoritesPage::retryQuery);
    stack_->addWidget(errorNotice_);

    auto* scroll = new QScrollArea(stack_);
    scroll->setObjectName(QStringLiteral("favoritesScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listPage_ = new QWidget(scroll);
    listLayout_ = new QVBoxLayout(listPage_);
    listLayout_->setContentsMargins(0, 0, 8, 0);
    listLayout_->setSpacing(10);
    scroll->setWidget(listPage_);
    stack_->addWidget(scroll);

    // 查询三态 → 状态门：started 关门、succeeded 开门、failed 置异常门
    // （context 传 this：页毁连接断）。
    connect(service_, &services::station::StationQueryService::queryStarted, this,
            &FavoritesPage::handleQueryStarted);
    connect(service_, &services::station::StationQueryService::querySucceeded, this,
            &FavoritesPage::handleQuerySucceeded);
    connect(service_, &services::station::StationQueryService::queryFailed, this,
            &FavoritesPage::handleQueryFailed);

    setFavoritesService(nullptr); // 自建兜底实例（HomeShell 注入同实例时替换）

    stack_->setCurrentWidget(loadingPage_);
    // 构造期预取一次全量候选（空关键词）：收藏列表 = 收藏 ID ∩ 该结果，
    // 不等用户操作首屏即出。
    service_->search(QString());
}

// 收藏服务注入（HomeShell 与首页星星同实例；空参 = 自建兜底，访客内存态）。
// 换实例必须先通配断旧再接新：漏断会出现「首页一个 toggle 触发两次 refresh」。
void FavoritesPage::setFavoritesService(FavoritesService* service)
{
    FavoritesService* target = service;
    if (target == nullptr) {
        if (favorites_ != nullptr) {
            return; // 已有自建兜底
        }
        target = new FavoritesService(this); // 未注入：访客内存态口径
    }
    // 幂等闸：同实例重复注入不重接信号、不额外 refresh。
    if (target == favorites_) {
        return;
    }
    // 通配断旧实例全部信号（不只 favoritesChanged）：换绑前旧源仍会触发刷新。
    if (favorites_ != nullptr) {
        disconnect(favorites_, nullptr, this, nullptr);
    }
    favorites_ = target;
    connect(favorites_, &FavoritesService::favoritesChanged, this,
            &FavoritesPage::handleFavoritesChanged);
    refresh();
}

void FavoritesPage::handleQueryStarted()
{
    queryLoaded_ = false; // 在途查询期间 refresh() 一律不得渲染空/列表态
    queryFailed_ = false;
    stack_->setCurrentWidget(loadingPage_);
}

// 唯一正常落定通道：开状态门（queryLoaded_）+ 缓存候选池，随后 refresh()
// 拼接渲染——空收藏也要走到这里才能出空态，而不是永远停在 loading。
void FavoritesPage::handleQuerySucceeded(const StationList& stations)
{
    queryFailed_ = false;
    queryLoaded_ = true;
    lastResults_ = stations;
    refresh();
}

// 失败落异常态：queryFailed_ 让状态门知道该保持错误页——旧候选池不清空，
// 但重试成功前不得被 refresh() 渲染出来（验收缺陷 4 的错误态分支）。
// 失败落异常态并保持：queryFailed_ 让状态门知道错误页不可被 refresh() 覆盖
// 成空态——否则「重试」入口永久丢失（验收缺陷 4 的另一半）。
void FavoritesPage::handleQueryFailed(const QString& message)
{
    queryLoaded_ = false;
    queryFailed_ = true;
    errorNotice_->setContent(QStringLiteral("⚠️"), tr("收藏列表加载失败"), message, tr("重试"));
    stack_->setCurrentWidget(errorNotice_);
}

void FavoritesPage::handleFavoritesChanged()
{
    // 首页星星/本页取消收藏都会走到这里；服务同步数据源，无在途请求冲突。
    refresh();
}

// 重试 = 重发查询；loading 视图由 queryStarted 回调统一置态，本函数不手动
// 抢切页面——保证状态门只有回调一个写入方，避免两处切页口径漂移。
void FavoritesPage::retryQuery()
{
    stack_->setCurrentWidget(loadingPage_);
    service_->search(QString());
}

// 收藏/站点资料两个数据源都要走这条重建路径：星星注入、查询落定、筛选应用、
// 重试成功后统一在此重拼列表。
void FavoritesPage::refresh()
{
    // 状态门（验收缺陷 4）：查询在途/失败时保持对应视图——壳层入口的无条件
    // refresh() 不得把加载覆盖成假“暂无收藏”，更不得把异常覆盖成空态、
    // 永久丢失“重试”入口。
    if (!queryLoaded_) {
        QWidget* target = queryFailed_ ? static_cast<QWidget*>(errorNotice_)
                                       : static_cast<QWidget*>(loadingPage_);
        stack_->setCurrentWidget(target);
        return;
    }
    // 收藏 ∩ 查询结果（服务顺序取反 = 最近收藏在前；ID 无对应站点跳过，
    // 见类注释 TODO(contract)）。
    const QVector<qint64> ids = favorites_ != nullptr ? favorites_->favoriteIds()
                                                      : QVector<qint64>{};
    StationList joined;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        for (const StationListItem& item : lastResults_) {
            if (item.station.id == *it) {
                joined.append(item);
                break;
            }
        }
    }
    // 筛选投影复用找站页同一纯函数 applyStationFilter：同条件模型保证两页
    // 命中口径一致（组内 OR / 组间 AND）。
    StationList visible = services::station::applyStationFilter(joined, filterCriteria_);

    // 整清重建（与找站页同款）：收藏/筛选变更低频，逐卡 diff 不值当；
    // item deleteLater 回收，在途信号栈里同步调用无悬空风险。
    clearLayoutItems(listLayout_);
    for (const StationListItem& item : visible) {
        listLayout_->addWidget(createFavoriteCard(item));
    }
    // 尾部弹性项：卡片顶部对齐，数量少时不被 layout 纵向拉散。
    listLayout_->addStretch();

    if (visible.isEmpty()) {
        // “收藏不为空但筛选无结果”与“本就无收藏”文案区分（规格：空态口径）。
        const bool filteredOut = !joined.isEmpty();
        emptyNotice_->setContent(
            QStringLiteral("⭐"),
            tr("暂无收藏的充电站"),
            filteredOut ? tr("当前筛选条件下没有收藏电站命中，试试放宽或重置筛选条件。")
                        : tr("在找站页点击卡片右下角 ☆ 即可收藏，收藏后在这里集中管理。"),
            QString());
        stack_->setCurrentWidget(emptyNotice_);
    } else {
        // stack_ 页 3 = 列表滚动页（0 loading/1 空/2 异常）。
        stack_->setCurrentIndex(3);
    }
}

QWidget* FavoritesPage::createFavoriteCard(const StationListItem& item)
{
    auto* card = new ClickableCard(listPage_);
    card->setProperty("isFavoriteCard", true);
    card->setProperty("stationId", item.station.id);
    card->setCursor(Qt::PointingHandCursor);

    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    auto* nameLabel = new QLabel(item.station.name, card);
    nameLabel->setProperty("role", QStringLiteral("sectionTitle"));
    nameLabel->setWordWrap(true);
    auto* availabilityTag = new StatusTag(
        item.station.availableChargers > 0
            ? tr("空闲 %1/%2").arg(item.station.availableChargers).arg(item.station.totalChargers)
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

    // 实心 ★：点击直接取消收藏（favoritesChanged → refresh 移除该卡片；
    // 按钮自身 deleteLater，clicked 发射栈内安全）。真按钮消费事件防误触详情。
    const qint64 stationId = item.station.id;
    auto* starButton = new QPushButton(QStringLiteral("★"), card);
    // objectName 内嵌站点 ID：探针按名定位特定站点的星星直接点击取消收藏
    // （整页星星无法按文本区分）。
    starButton->setObjectName(QStringLiteral("favoriteCardStar_%1").arg(stationId));
    starButton->setProperty("isStationStar", true);
    starButton->setCursor(Qt::PointingHandCursor);
    starButton->setToolTip(tr("取消收藏"));
    starButton->setAccessibleName(tr("取消收藏"));
    connect(starButton, &QPushButton::clicked, this,
            [this, stationId]() {
                if (favorites_ != nullptr) {
                    favorites_->toggle(stationId);
                }
            });

    detailRow->addWidget(priceLabel);
    detailRow->addStretch();
    detailRow->addWidget(distanceLabel);
    detailRow->addWidget(starButton);
    body->addLayout(detailRow);

    // 取行数据本地副本再值捕获：卡片点击远晚于 lastResults_ 下一次整清重建，
    // 直接捕获 item 引用会悬空。
    const charging::model::Station station = item.station;
    const int distanceMeters = item.distanceMeters;
    connect(card, &ClickableCard::clicked, this,
            [this, station, distanceMeters]() { emit stationSelected(station, distanceMeters); });

    return card;
}

// QPointer 去重：弹窗 WA_DeleteOnClose 关闭即自毁、QPointer 自动置空，
// 因此重复进入只需把在开的弹窗提前台——不会出现第二个条件编辑窗。
void FavoritesPage::openFilterDialog()
{
    if (filterDialog_ != nullptr) {
        filterDialog_->raise();
        filterDialog_->activateWindow();
        return;
    }
    auto* dialog = new StationFilterDialog(filterCriteria_, this);
    connect(dialog, &StationFilterDialog::applied, this, &FavoritesPage::setFilterCriteria);
    filterDialog_ = dialog;
    dialog->show();
}

void FavoritesPage::setFilterCriteria(const StationFilterCriteria& criteria)
{
    filterCriteria_ = criteria;
    // 筛选按钮是构造期局部变量、未留成员：按 objectName 找回改高亮属性，
    // 为一个引用专门加成员不划算。
    auto* button = findChild<QPushButton*>(QStringLiteral("favoritesFilterButton"));
    if (button != nullptr) {
        button->setProperty("filterActive", !criteria.isEmpty());
        // 属性选择器变化需重设样式串才生效（Qt 已知口径）。
        button->style()->polish(button);
    }
    refresh();
}

int FavoritesPage::favoriteCardCount() const
{
    // 按 isFavoriteCard 属性计数：排除尾部弹性项与占位，属性即页面-测试契约。
    int count = 0;
    for (int i = 0; i < listLayout_->count(); ++i) {
        const QLayoutItem* item = listLayout_->itemAt(i);
        if (item != nullptr && item->widget() != nullptr
            && item->widget()->property("isFavoriteCard").toBool()) {
            ++count;
        }
    }
    return count;
}

bool FavoritesPage::emptyStateVisible() const
{
    // 双门断言：当前页是空提示 且 提示自身可见——整页未上屏时不误报可见。
    return stack_->currentWidget() == emptyNotice_ && emptyNotice_->isVisible();
}

FavoritesPage::ViewState FavoritesPage::viewState() const
{
    // 探针由状态门字段推导而非 stack_ 当前页：Loading/Error 的语义本就住在
    // queryLoaded_/queryFailed_ 两 flag 里，读当前页反而绕。
    if (!queryLoaded_) {
        return queryFailed_ ? ViewState::Error : ViewState::Loading;
    }
    return stack_->currentWidget() == emptyNotice_ ? ViewState::Empty : ViewState::List;
}

} // namespace charging::client::pages::station
