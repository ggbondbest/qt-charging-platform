// StationDetailPage 实现（成员 2；任务 #12/#17 及迭代批）。数据流：
// openStation() → StationQueryService::fetchDetail（模拟 ↔ TCP 真实通道对 UI
// 透明）→ detail* 三态信号驱动整页切换；桩卡片上的预约点击在页内完成
// 登录/车辆/名额三级拦截判定，放行才发 reservationConfirmRequested 交宿主
// 路由独立确认页（#17 迭代起废弃弹窗式预约）。
#include "pages/station/station_detail_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"
#include "services/reservation/reservation_service.h"
#include "services/settings/settings_service.h"
#include "services/station/station_query_service.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace charging::client::pages::station {

namespace {

// 页面局部样式（token 与全局主题一致，仅本页生效，不改全局 QSS）。
const char* kDetailPageStyleSheet = R"(
QLabel#detailOfflineBanner {
    background: #FFF4E5;
    border: 1px solid #F5A623;
    border-radius: 10px;
    color: #8A5A00;
    font-size: 13px;
    font-weight: 600;
    padding: 10px 12px;
}
QFrame[chargerFault="true"] {
    border: 2px solid #E5484D;
    background: #FFF5F5;
}
QPushButton#detailReserveButton {
    background: #00B578;
    color: #FFFFFF;
    border: none;
    border-radius: 14px;
    padding: 5px 14px;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#detailReserveButton:pressed {
    background: #009A66;
}
QPushButton#detailReserveButton:disabled {
    background: #F4F6F8;
    color: #9AA5B1;
    border: 1px solid #D5DCE4;
}
)";

// 电价展示口径：域内整数分/度（centsPerKwh）传数，仅在 UI 显示边缘换算成
// 元并定 2 位小数——页内不做任何价格计算，避免浮点误差混进业务数据。
QString formatPrice(qint64 centsPerKwh)
{
    return QStringLiteral("¥%1/度").arg(QString::number(centsPerKwh / 100.0, 'f', 2));
}

// 虚拟测距口径：负数 = 未知距离（如尚未拿到快照），显示 “--” 占位比
// “0 m” 更诚实；≥1000 自动升 km（1 位小数）。
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

struct ChargerStatusView
{
    const char* text;
    StatusTag::Tone tone;
};

// 工作状态 → 展示文案与色调（空闲/占用/离线/故障四类视觉语义）。
ChargerStatusView statusView(charging::model::ChargerStatus status)
{
    using charging::model::ChargerStatus;
    switch (status) {
    case ChargerStatus::Available:
        return {"空闲", StatusTag::Tone::Success};
    case ChargerStatus::Charging:
        return {"占用·充电中", StatusTag::Tone::Warning};
    case ChargerStatus::Reserved:
        return {"占用·已预约", StatusTag::Tone::Info};
    case ChargerStatus::Fault:
        return {"故障", StatusTag::Tone::Danger};
    case ChargerStatus::Offline:
        return {"离线", StatusTag::Tone::Neutral};
    }
    return {"未知", StatusTag::Tone::Neutral};
}

// 布局整清：item 逐个 takeAt 后 delete，widget 走 deleteLater——本函数会在
// 服务信号槽栈内被同步调用（handleDetailSucceeded 重建列表），立即 delete
// 有悬空风险，deleteLater 交事件循环回收。
void clearLayoutItems(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

StationDetailPage::StationDetailPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("stationDetailPage"));
    setStyleSheet(QString::fromLatin1(kDetailPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("站点详情"), this);
    titleLabel->setObjectName(QStringLiteral("detailPageTitle"));
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    rootLayout->addWidget(titleLabel);

    pageStack_ = new QStackedWidget(this);
    pageStack_->setObjectName(QStringLiteral("detailPageStack"));
    rootLayout->addWidget(pageStack_, 1);

    // ① 加载中（整页）：进入详情即拉取桩列表，规格禁止出现大片空白。
    loadingPage_ = new QWidget(pageStack_);
    auto* loadingLayout = new QVBoxLayout(loadingPage_);
    auto* loadingLabel = new QLabel(tr("⏳ 正在加载站点详情…"), loadingPage_);
    loadingLabel->setObjectName(QStringLiteral("detailLoadingLabel"));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setProperty("role", QStringLiteral("secondary"));
    loadingLayout->addStretch();
    loadingLayout->addWidget(loadingLabel);
    loadingLayout->addStretch();
    pageStack_->addWidget(loadingPage_);

    // ② 错误态：站点 ID 非法 / 接口异常 / 网络错误 → 友好提示 + 返回首页。
    errorNotice_ = new NoticePanel(QStringLiteral("⚠️"), tr("站点详情加载失败"), QString(),
                                   tr("返回首页"), pageStack_);
    errorNotice_->setObjectName(QStringLiteral("detailErrorNotice"));
    connect(errorNotice_, &NoticePanel::actionTriggered, this,
            [this]() { emit backRequested(); });
    pageStack_->addWidget(errorNotice_);

    // ③ 正常态：信息卡 + 离线横幅 + 桩列表（内部再分 空/正常）。
    auto* contentPage = new QWidget(pageStack_);
    auto* contentLayout = new QVBoxLayout(contentPage);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);
    pageStack_->addWidget(contentPage);

    auto* infoCard = new Card(contentPage);
    infoCard->setProperty("isDetailInfoCard", true);
    auto* body = infoCard->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    nameLabel_ = new QLabel(contentPage);
    nameLabel_->setObjectName(QStringLiteral("detailNameLabel"));
    nameLabel_->setProperty("role", QStringLiteral("sectionTitle"));
    // 不换行的长文本会撑宽整页最小尺寸，导致右侧（状态标签/按钮列）被裁切。
    nameLabel_->setWordWrap(true);
    statusTag_ = new StatusTag(QString(), StatusTag::Tone::Success, contentPage);
    statusTag_->setObjectName(QStringLiteral("detailStatusTag"));
    titleRow->addWidget(nameLabel_);
    titleRow->addStretch();
    titleRow->addWidget(statusTag_);
    body->addLayout(titleRow);

    addressLabel_ = new QLabel(contentPage);
    addressLabel_->setObjectName(QStringLiteral("detailAddressLabel"));
    addressLabel_->setProperty("role", QStringLiteral("secondary"));
    addressLabel_->setWordWrap(true);
    body->addWidget(addressLabel_);

    auto* priceRow = new QHBoxLayout();
    priceLabel_ = new QLabel(contentPage);
    priceLabel_->setObjectName(QStringLiteral("detailPriceLabel"));
    priceLabel_->setProperty("role", QStringLiteral("amountStrong"));
    distanceLabel_ = new QLabel(contentPage);
    distanceLabel_->setObjectName(QStringLiteral("detailDistanceLabel"));
    distanceLabel_->setProperty("role", QStringLiteral("secondary"));
    priceRow->addWidget(priceLabel_);
    priceRow->addStretch();
    priceRow->addWidget(distanceLabel_);
    body->addLayout(priceRow);
    contentLayout->addWidget(infoCard);

    // 站点整体离线：醒目横幅（数据源驱动，仅 Inactive 时展示）。
    offlineBanner_ = new QLabel(tr("⚠️ 该站点当前处于离线状态，暂不可用，请稍后再试或选择其他站点"),
                                contentPage);
    offlineBanner_->setObjectName(QStringLiteral("detailOfflineBanner"));
    offlineBanner_->setWordWrap(true);
    offlineBanner_->hide();
    contentLayout->addWidget(offlineBanner_);

    chargerSummaryLabel_ = new QLabel(contentPage);
    chargerSummaryLabel_->setObjectName(QStringLiteral("detailChargerSummaryLabel"));
    chargerSummaryLabel_->setProperty("role", QStringLiteral("sectionTitle"));
    contentLayout->addWidget(chargerSummaryLabel_);

    chargerStack_ = new QStackedWidget(contentPage);
    chargerStack_->setObjectName(QStringLiteral("detailChargerStack"));
    contentLayout->addWidget(chargerStack_, 1);

    chargerEmptyNotice_ = new NoticePanel(QStringLiteral("🔌"), tr("该站点暂无充电桩"),
                                          tr("站点信息已展示；桩位尚未录入，暂无法预约或充电。"),
                                          QString(), chargerStack_);
    chargerEmptyNotice_->setObjectName(QStringLiteral("detailChargerEmptyNotice"));
    chargerStack_->addWidget(chargerEmptyNotice_);

    auto* chargerScroll = new QScrollArea(chargerStack_);
    chargerScroll->setObjectName(QStringLiteral("detailChargerScroll"));
    chargerScroll->setWidgetResizable(true);
    chargerScroll->setFrameShape(QFrame::NoFrame);
    chargerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chargerListPage_ = new QWidget(chargerScroll);
    chargerListLayout_ = new QVBoxLayout(chargerListPage_);
    chargerListLayout_->setContentsMargins(0, 0, 8, 0);
    chargerListLayout_->setSpacing(8);
    chargerScroll->setWidget(chargerListPage_);
    chargerStack_->addWidget(chargerScroll);

    // 初始为整页加载中：openStation() 拉取桩列表后进入 Ready（或 Error）。
    setDetailState(DetailState::Loading);
}

// 详情通道注入（非拥有，与找站页共用 HomeShell 的同一实例）。幂等闸：同实例
// 重复注入直接返回——漏闸会让重复 connect 把一次服务信号送进处理函数多次
// （列表被无谓重建）。context 传 this：页面销毁时连接自动断开。
void StationDetailPage::setService(services::station::StationQueryService* service)
{
    if (service_ == service) {
        return;
    }
    service_ = service;
    if (service_ != nullptr) {
        connect(service_, &services::station::StationQueryService::detailStarted, this,
                &StationDetailPage::handleDetailStarted);
        connect(service_, &services::station::StationQueryService::detailSucceeded, this,
                &StationDetailPage::handleDetailSucceeded);
        connect(service_, &services::station::StationQueryService::detailFailed, this,
                &StationDetailPage::handleDetailFailed);
    }
}

// 页面唯一入口（宿主路由“查看站点”调入）：先用列表页带入的路由快照即时
// 渲染信息区，再交服务异步拉桩列表——两路数据分阶段回写，页面不等服务即
// 可露出站名/地址/电价；distanceMeters 为列表页测得的展示值，原样透传。
void StationDetailPage::openStation(const charging::model::Station& station, int distanceMeters)
{
    station_ = station;
    lastDistanceMeters_ = distanceMeters;

    nameLabel_->setText(station.name);
    addressLabel_->setText(station.address);
    priceLabel_->setText(formatPrice(station.priceCentsPerKwh));
    distanceLabel_->setText(tr("距您 %1").arg(formatDistance(distanceMeters)));
    const bool active = station.status == charging::model::StationStatus::Active;
    statusTag_->setText(active ? tr("营业中") : tr("已离线"));
    statusTag_->setTone(active ? StatusTag::Tone::Success : StatusTag::Tone::Danger);
    offlineBanner_->setVisible(!active);
    chargerSummaryLabel_->setText(tr("充电桩"));

    // 信息区先展示路由快照，桩列表异步拉取（无服务注入时保持加载中）。
    setDetailState(DetailState::Loading);
    if (service_ != nullptr) {
        service_->fetchDetail(station, distanceMeters);
    }
}

// 登录态注入（纯 setter，默认 true=宿主未接线时不拦演示路径）：预约闸门
// 第一级在点击瞬间同步读取它，未登录只发 reservationLoginRequired 交宿主。
void StationDetailPage::setLoggedIn(bool loggedIn)
{
    loggedIn_ = loggedIn;
}

// 预约/车辆档案注入（非拥有）：纯 setter 无信号接线——名额与匹配判定都在
// 按钮点击瞬间同步读取，避免为瞬时查询维护订阅生命周期。
void StationDetailPage::setReservationService(
    services::reservation::ReservationService* service)
{
    reservationService_ = service;
}

void StationDetailPage::setSettingsService(
    services::settings::SettingsService* settings)
{
    settings_ = settings;
}

bool StationDetailPage::matchesDefaultVehicle(const charging::model::Charger& charger) const
{
    // 无车辆档案（未接入设置服务或尚未添加车辆）时不做接口类型筛选。
    if (settings_ == nullptr) {
        return true;
    }
    const auto* vehicle = settings_->defaultVehicle();
    return vehicle == nullptr || vehicle->connectorType == charger.type;
}

// ---- 测试探针（isVisibleTo 语义：整页未被宿主显示也能断言，见 .h） ----

StationDetailPage::DetailState StationDetailPage::viewState() const
{
    return viewState_;
}

int StationDetailPage::chargerCardCount() const
{
    // 按 isChargerCard 属性计数而非 layout 总项数：属性是页面-测试契约，
    // 天然排除占位/间隔等非卡片项。
    int count = 0;
    for (int i = 0; i < chargerListLayout_->count(); ++i) {
        const auto* item = chargerListLayout_->itemAt(i);
        if (item != nullptr && item->widget() != nullptr
            && item->widget()->property("isChargerCard").toBool()) {
            ++count;
        }
    }
    return count;
}

bool StationDetailPage::offlineBannerVisible() const
{
    return offlineBanner_->isVisibleTo(this);
}

bool StationDetailPage::chargerEmptyVisible() const
{
    // 列表区当前页是否为“暂无充电桩”提示。
    return chargerEmptyNotice_->isVisibleTo(this);
}

// ① 三态切换唯一入口：viewState_（探针口径）与 pageStack_ 当前页同步变更，
// 其它地方不绕过它直接 setCurrentIndex，保证两者永不漂移。
void StationDetailPage::setDetailState(DetailState state)
{
    viewState_ = state;
    switch (state) {
    case DetailState::Loading:
        pageStack_->setCurrentWidget(loadingPage_);
        break;
    case DetailState::Error:
        pageStack_->setCurrentWidget(errorNotice_);
        break;
    case DetailState::Ready:
        pageStack_->setCurrentIndex(2); // contentPage
        break;
    }
}

// 服务 detailStarted 回灌：任何一次详情拉取（openStation 进入、预约置位后
// noteChargerReserved 重拉）都由服务侧信号统一置加载态；openStation 里的
// 本地置位只兜“未注入服务”的场景。
void StationDetailPage::handleDetailStarted()
{
    setDetailState(DetailState::Loading);
}

void StationDetailPage::handleDetailSucceeded(const services::station::StationDetail& detail)
{
    // 以服务端返回的站点数据为准回写信息区（离线/空桩等状态由数据源决定，
    // UI 不伪造；路由快照在等待期间可能已过期）。
    station_ = detail.station;
    nameLabel_->setText(detail.station.name);
    addressLabel_->setText(detail.station.address);
    priceLabel_->setText(formatPrice(detail.station.priceCentsPerKwh));
    const bool active = detail.station.status == charging::model::StationStatus::Active;
    statusTag_->setText(active ? tr("营业中") : tr("已离线"));
    statusTag_->setTone(active ? StatusTag::Tone::Success : StatusTag::Tone::Danger);
    offlineBanner_->setVisible(!active);

    clearChargerRows();
    // 任务 #17 二次迭代：与默认车辆接口类型匹配的“空闲”桩排在前面
    // （stable_partition 保持其余相对顺序，接口不匹配的桩仍展示可选）。
    auto chargers = detail.chargers;
    const auto isTopPriority = [this](const charging::model::Charger& charger) {
        return charger.status == charging::model::ChargerStatus::Available
            && matchesDefaultVehicle(charger);
    };
    std::stable_partition(chargers.begin(), chargers.end(), isTopPriority);
    for (const auto& charger : chargers) {
        chargerListLayout_->addWidget(createChargerCard(charger));
    }
    const int available = detail.station.availableChargers;
    chargerSummaryLabel_->setText(tr("充电桩（空闲 %1 / 共 %2）")
                                      .arg(available)
                                      .arg(detail.chargers.size()));
    if (detail.chargers.isEmpty()) {
        // 空数据状态：站点正常但暂无桩位。
        chargerStack_->setCurrentWidget(chargerEmptyNotice_);
    } else {
        // chargerStack_ 页 1 = 桩列表滚动页（页 0 = 空数据提示）。
        chargerStack_->setCurrentIndex(1);
    }
    setDetailState(DetailState::Ready);
}

void StationDetailPage::handleDetailFailed(const QString& message)
{
    // 接口异常 / 网络错误 / 站点 ID 非法：友好提示 + “返回首页”回找站列表。
    errorNotice_->setContent(QStringLiteral("⚠️"), tr("站点详情加载失败"), message,
                             tr("返回首页"));
    setDetailState(DetailState::Error);
}

QWidget* StationDetailPage::createChargerCard(const charging::model::Charger& charger)
{
    auto* card = new ClickableCard(chargerListPage_);
    card->setProperty("isChargerCard", true);
    // id/状态以枚举原值写入动态属性：供测试探针逐桩断言，也让故障红框样式
    // 走属性选择器（chargerFault）而非字符串比较，避免文案漂移破坏契约。
    card->setProperty("chargerId", charger.id);
    card->setProperty("chargerStatus", static_cast<int>(charger.status));
    const bool fault = charger.status == charging::model::ChargerStatus::Fault;
    if (fault) {
        // 故障视觉标记：页面局部样式按属性选择器渲染红色边框。
        card->setProperty("chargerFault", true);
    }
    card->setAccessibleName(charger.code);

    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    auto* codeLabel = new QLabel(charger.code, card);
    codeLabel->setProperty("role", QStringLiteral("sectionTitle"));
    const auto view = statusView(charger.status);
    // 文案表用 const char*（编译期表、无静态初始化负担），构造时再套 tr()
    // 取译文——Qt 翻译只认 tr() 包裹的运行时字符串。
    auto* tag = new StatusTag(tr(view.text), view.tone, card);
    tag->setObjectName(QStringLiteral("chargerStatusTag"));
    titleRow->addWidget(codeLabel);
    titleRow->addStretch();
    titleRow->addWidget(tag);
    body->addLayout(titleRow);

    const bool fast = charger.type == charging::model::ChargerType::Fast;
    auto* specRow = new QHBoxLayout();
    auto* typeLabel = new QLabel(fast ? tr("直流快充") : tr("交流慢充"), card);
    typeLabel->setProperty("role", QStringLiteral("secondary"));
    auto* powerLabel =
        new QLabel(QStringLiteral("%1 kW").arg(charger.powerWatts / 1000), card);
    powerLabel->setProperty("role", QStringLiteral("secondary"));
    specRow->addWidget(typeLabel);
    specRow->addSpacing(10);
    specRow->addWidget(powerLabel);
    specRow->addStretch();

    // 预约入口（任务 #17）：所有桩展示按钮；非空闲置灰不可点击（权限控制）。
    auto* reserveButton = new QPushButton(tr("预约"), card);
    reserveButton->setObjectName(QStringLiteral("detailReserveButton"));
    const bool reservable = charger.status == charging::model::ChargerStatus::Available;
    reserveButton->setEnabled(reservable);
    if (reservable) {
        reserveButton->setCursor(Qt::PointingHandCursor);
        if (!matchesDefaultVehicle(charger)) {
            // 接口类型与默认车辆不匹配：仅提示，不禁止选择（用户可换车）。
            reserveButton->setToolTip(tr("与默认车辆接口不匹配，预约前请确认车辆"));
        }
    } else {
        reserveButton->setToolTip(tr("仅空闲充电桩可预约"));
    }
    connect(reserveButton, &QPushButton::clicked, this,
            // 值捕获 charger：形参生命周期只在本次建卡调用栈内，按钮点击远晚
            // 于此，捕获引用必悬空。
            [this, charger]() { handleReserveRequested(charger); });
    specRow->addWidget(reserveButton);
    body->addLayout(specRow);

    return card;
}

// 预约入口三级闸门（登录 → 车辆 → 名额），任一拦截即止并交宿主提示；全过
// 才发 reservationConfirmRequested。判定全在页内同步读取（不订阅服务信号），
// 点击瞬间取最新状态。
void StationDetailPage::handleReserveRequested(const charging::model::Charger& charger)
{
    // 观察信号先行发出：无论后续走拦截还是放行，宿主埋点/测试都能确定性地
    // 拿到“点了哪根桩”。
    emit reservationRequested(charger.id);

    if (!loggedIn_) {
        // 未登录拦截：交宿主（HomeShell）提示登录并跳转登录页。
        emit reservationLoginRequired();
        return;
    }

    // 任务 #17 二次迭代：预约名额由车辆决定——账号下无车辆时无法发起
    // 预约，交宿主提示引导去「设置 - 车辆管理」添加。
    // liveMode 豁免：真实通道的名额/车辆约束以服务端为准，本机档案只用于
    // 模拟通道的入口拦截，避免拿不可靠的本地状态误挡真实用户。
    if (settings_ != nullptr && settings_->vehicleCount() <= 0
        && !(reservationService_ && reservationService_->liveMode())) {
        emit reservationVehicleRequired();
        return;
    }

    // 名额制业务约束：有效预约数已达车辆数上限时拦截新建，交宿主弹提示，
    // 不进入预约确认页面（Service 层提交时仍会二次校验，防绕过）。
    if (reservationService_ != nullptr
        && reservationService_->activeReservationCount()
            >= reservationService_->unfinishedSlotLimit()) {
        emit reservationBlocked();
        return;
    }

    // 满足预约条件：交宿主路由至独立预约确认页面（不再使用弹窗）。
    emit reservationConfirmRequested(station_, charger, lastDistanceMeters_);
}

void StationDetailPage::noteChargerReserved(qint64 chargerId)
{
    // 预约成功 → 刷新当前充电桩状态（模拟通道本地置为已预约后重拉；
    // 真实通道由服务端数据体现，override 仅作用于模拟结果）。
    if (service_ != nullptr) {
        service_->setMockChargerReserved(chargerId);
        service_->fetchDetail(station_, lastDistanceMeters_);
    }
}

void StationDetailPage::clearChargerRows()
{
    clearLayoutItems(chargerListLayout_);
}

} // namespace charging::client::pages::station
