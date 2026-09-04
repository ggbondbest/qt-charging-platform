#include "pages/station/station_detail_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"
#include "services/reservation/reservation_service.h"
#include "services/station/station_query_service.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVariant>
#include <QVBoxLayout>

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
    statusTag_ = new StatusTag(QString(), StatusTag::Tone::Success, contentPage);
    statusTag_->setObjectName(QStringLiteral("detailStatusTag"));
    titleRow->addWidget(nameLabel_);
    titleRow->addStretch();
    titleRow->addWidget(statusTag_);
    body->addLayout(titleRow);

    addressLabel_ = new QLabel(contentPage);
    addressLabel_->setObjectName(QStringLiteral("detailAddressLabel"));
    addressLabel_->setProperty("role", QStringLiteral("secondary"));
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
    chargerListPage_ = new QWidget(chargerScroll);
    chargerListLayout_ = new QVBoxLayout(chargerListPage_);
    chargerListLayout_->setContentsMargins(0, 0, 8, 0);
    chargerListLayout_->setSpacing(8);
    chargerScroll->setWidget(chargerListPage_);
    chargerStack_->addWidget(chargerScroll);

    // 初始为整页加载中：openStation() 拉取桩列表后进入 Ready（或 Error）。
    setDetailState(DetailState::Loading);
}

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

void StationDetailPage::setLoggedIn(bool loggedIn)
{
    loggedIn_ = loggedIn;
}

void StationDetailPage::setReservationService(
    services::reservation::ReservationService* service)
{
    reservationService_ = service;
}

StationDetailPage::DetailState StationDetailPage::viewState() const
{
    return viewState_;
}

int StationDetailPage::chargerCardCount() const
{
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
    for (const auto& charger : detail.chargers) {
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
    } else {
        reserveButton->setToolTip(tr("仅空闲充电桩可预约"));
    }
    connect(reserveButton, &QPushButton::clicked, this,
            [this, charger]() { handleReserveRequested(charger); });
    specRow->addWidget(reserveButton);
    body->addLayout(specRow);

    return card;
}

void StationDetailPage::handleReserveRequested(const charging::model::Charger& charger)
{
    emit reservationRequested(charger.id);

    if (!loggedIn_) {
        // 未登录拦截：交宿主（HomeShell）提示登录并跳转登录页。
        emit reservationLoginRequired();
        return;
    }

    // 业务约束（任务 #17 迭代）：存在未结束预约时拦截新建，交宿主弹提示，
    // 不进入预约确认页面（Service 层提交时仍会二次校验，防绕过）。
    if (reservationService_ != nullptr && reservationService_->hasUnfinishedReservation()) {
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
