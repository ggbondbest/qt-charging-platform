#include "pages/station/reservation_confirm_page.h"

#include "charging/client/widgets/card.h"
#include "pages/station/platform_theme.h"
#include "services/reservation/reservation_service.h"
#include "services/settings/settings_service.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

using services::reservation::ReservationService;

// 与 Service 层一致的业务上限（UI 先行拦截 + Service 兜底，防绕过）。
constexpr int kMaxSlotMinutes = 45;

// 页面局部样式：电动绿 token 与全局主题一致，仅本页生效，不改全局 QSS。
const char* kConfirmPageStyleSheet = R"(
QWidget#reservationConfirmPage {
    background: #F7F9FB;
}
QLabel#reservationConfirmTitle {
    color: #1F2937;
    font-size: 16px;
    font-weight: 700;
}
QPushButton#reservationConfirmButton {
    background: #00B578;
    color: #FFFFFF;
    border: none;
    border-radius: 16px;
    padding: 8px 22px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#reservationConfirmButton:pressed {
    background: #009A66;
}
QPushButton#reservationConfirmButton:disabled {
    background: #B9C4CF;
}
QPushButton#reservationCloseButton {
    background: #F4F6F8;
    color: #1F2937;
    border: 1px solid #D5DCE4;
    border-radius: 16px;
    padding: 8px 22px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#useRecommendedSlotButton {
    background: #E6F7F0;
    color: #00795A;
    border: 1px solid #7FD4B8;
    border-radius: 14px;
    padding: 6px 14px;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#useRecommendedSlotButton:pressed {
    background: #D2F0E4;
}
QLabel#reservationFeeLabel {
    color: #00A76D;
    font-size: 14px;
    font-weight: 700;
}
QLabel#reservationMessageLabel {
    font-size: 12px;
    font-weight: 600;
}
)";

} // namespace

ReservationConfirmPage::ReservationConfirmPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("reservationConfirmPage"));
    setStyleSheet(QString::fromLatin1(kConfirmPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("预约确认"), this);
    titleLabel->setObjectName(QStringLiteral("reservationConfirmTitle"));
    rootLayout->addWidget(titleLabel);

    // 长内容滚动容器：鼠标滚轮上下滚动（规格通用要求）。
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("reservationConfirmScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rootLayout->addWidget(scroll, 1);

    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 8, 0);
    contentLayout->setSpacing(10);
    scroll->setWidget(content);

    auto* card = new Card(content);
    card->setProperty("isReservationConfirmCard", true);
    auto* body = card->bodyLayout();

    auto addRow = [&](const QString& caption, const QString& value, const QString& objectName) {
        auto* row = new QHBoxLayout();
        auto* captionLabel = new QLabel(caption, card);
        captionLabel->setProperty("role", QStringLiteral("secondary"));
        auto* valueLabel = new QLabel(value, card);
        valueLabel->setObjectName(objectName);
        valueLabel->setProperty("role", QStringLiteral("sectionTitle"));
        valueLabel->setWordWrap(true);
        row->addWidget(captionLabel);
        row->addStretch();
        row->addWidget(valueLabel);
        body->addLayout(row);
        return valueLabel;
    };

    stationNameLabel_ = addRow(tr("站点名称"), QString(),
                               QStringLiteral("confirmStationNameLabel"));
    chargerCodeLabel_ = addRow(tr("充电桩编号"), QString(),
                               QStringLiteral("confirmChargerCodeLabel"));
    chargerSpecLabel_ = addRow(tr("充电类型 / 功率"), QString(),
                               QStringLiteral("confirmChargerSpecLabel"));

    // —— 预约车辆下拉（数据源：设置-车辆管理）——
    auto* vehicleRow = new QHBoxLayout();
    auto* vehicleCaption = new QLabel(tr("预约车辆"), card);
    vehicleCaption->setProperty("role", QStringLiteral("secondary"));
    vehicleComboBox_ = new QComboBox(card);
    vehicleComboBox_->setObjectName(QStringLiteral("reservationVehicleComboBox"));
    vehicleRow->addWidget(vehicleCaption);
    vehicleRow->addStretch();
    vehicleRow->addWidget(vehicleComboBox_);
    body->addLayout(vehicleRow);

    // —— 预约时间段：开始 / 结束 ——
    const auto addTimeRow = [&](const QString& caption, const QString& objectName) {
        auto* row = new QHBoxLayout();
        auto* captionLabel = new QLabel(caption, card);
        captionLabel->setProperty("role", QStringLiteral("secondary"));
        auto* edit = new QDateTimeEdit(card);
        edit->setObjectName(objectName);
        edit->setDisplayFormat(QStringLiteral("MM月dd日 HH:mm"));
        edit->setCalendarPopup(true);
        row->addWidget(captionLabel);
        row->addStretch();
        row->addWidget(edit);
        body->addLayout(row);
        return edit;
    };
    startEdit_ = addTimeRow(tr("开始时间"), QStringLiteral("reservationStartEdit"));
    endEdit_ = addTimeRow(tr("结束时间"), QStringLiteral("reservationEndEdit"));

    // —— 系统推荐时段（行驶时长为模拟估算，真实地图 API 就绪后替换）——
    auto* recommendedRow = new QHBoxLayout();
    recommendedButton_ = new QPushButton(tr("✨ 使用系统推荐时段"), card);
    recommendedButton_->setObjectName(QStringLiteral("useRecommendedSlotButton"));
    recommendedButton_->setCursor(Qt::PointingHandCursor);
    recommendedRow->addWidget(recommendedButton_);
    recommendedRow->addStretch();
    body->addLayout(recommendedRow);

    feeLabel_ = new QLabel(card);
    feeLabel_->setObjectName(QStringLiteral("reservationFeeLabel"));
    feeLabel_->setWordWrap(true);
    body->addWidget(feeLabel_);
    contentLayout->addWidget(card);
    contentLayout->addStretch();

    messageLabel_ = new QLabel(this);
    messageLabel_->setObjectName(QStringLiteral("reservationMessageLabel"));
    messageLabel_->setWordWrap(true);
    messageLabel_->hide();
    rootLayout->addWidget(messageLabel_);

    auto* buttonRow = new QHBoxLayout();
    auto* closeButton = new QPushButton(tr("关闭"), this);
    closeButton->setObjectName(QStringLiteral("reservationCloseButton"));
    closeButton->setCursor(Qt::PointingHandCursor);
    confirmButton_ = new QPushButton(tr("确认预约"), this);
    confirmButton_->setObjectName(QStringLiteral("reservationConfirmButton"));
    confirmButton_->setCursor(Qt::PointingHandCursor);
    buttonRow->addWidget(closeButton);
    buttonRow->addStretch();
    buttonRow->addWidget(confirmButton_);
    rootLayout->addLayout(buttonRow);

    connect(closeButton, &QPushButton::clicked, this, &ReservationConfirmPage::closeRequested);
    connect(confirmButton_, &QPushButton::clicked, this, &ReservationConfirmPage::handleSubmit);
    connect(recommendedButton_, &QPushButton::clicked, this,
            &ReservationConfirmPage::applyRecommendedSlot);
    connect(vehicleComboBox_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateSlotValidity(); });
    connect(startEdit_, &QDateTimeEdit::dateTimeChanged, this,
            [this](const QDateTime&) { updateSlotValidity(); });
    connect(endEdit_, &QDateTimeEdit::dateTimeChanged, this,
            [this](const QDateTime&) { updateSlotValidity(); });
}

void ReservationConfirmPage::setService(services::reservation::ReservationService* service)
{
    if (service_ == service) {
        return;
    }
    service_ = service;
    if (service_ != nullptr) {
        connect(service_, &ReservationService::submitStarted, this,
                &ReservationConfirmPage::handleSubmitStarted);
        connect(service_, &ReservationService::submitSucceeded, this,
                &ReservationConfirmPage::handleSubmitSucceeded);
        connect(service_, &ReservationService::submitFailed, this,
                &ReservationConfirmPage::handleSubmitFailed);
    }
}

void ReservationConfirmPage::setSettingsService(
    services::settings::SettingsService* settings)
{
    if (settings_ == settings) {
        return;
    }
    settings_ = settings;
    if (settings_ != nullptr) {
        // 车辆增删改实时联动下拉（设置页与本页共用同一 SettingsService）。
        connect(settings_, &services::settings::SettingsService::vehiclesChanged, this,
                &ReservationConfirmPage::refreshVehicles);
    }
    refreshVehicles();
}

void ReservationConfirmPage::openContext(const charging::model::Station& station,
                                         const charging::model::Charger& charger,
                                         int distanceMeters)
{
    station_ = station;
    charger_ = charger;
    distanceMeters_ = distanceMeters;

    stationNameLabel_->setText(station_.name);
    chargerCodeLabel_->setText(charger_.code);
    const bool fast = charger_.type == charging::model::ChargerType::Fast;
    chargerSpecLabel_->setText(
        tr("%1 · %2 kW").arg(fast ? tr("直流快充") : tr("交流慢充"))
            .arg(charger_.powerWatts / 1000));

    // 复位表单与提示：每次进入都是一次全新预约，默认填入系统推荐时段。
    refreshVehicles();
    applyRecommendedSlot();
    messageLabel_->hide();
    submitting_ = false;
    resetSubmitButton();
    updateSlotValidity();
}

ReservationConfirmPage::PageState ReservationConfirmPage::pageState() const
{
    return submitting_ ? PageState::Submitting : PageState::Idle;
}

int ReservationConfirmPage::selectedMinutes() const
{
    return int(startEdit_->dateTime().toUTC().secsTo(endEdit_->dateTime().toUTC()) / 60);
}

qint64 ReservationConfirmPage::selectedVehicleId() const
{
    return vehicleComboBox_->currentData().toLongLong();
}

QDateTime ReservationConfirmPage::startUtc() const
{
    return startEdit_->dateTime().toUTC();
}

QDateTime ReservationConfirmPage::endUtc() const
{
    return endEdit_->dateTime().toUTC();
}

QString ReservationConfirmPage::estimatedFeeText() const
{
    return feeLabel_->text();
}

QString ReservationConfirmPage::messageText() const
{
    return messageLabel_->text();
}

QString ReservationConfirmPage::recommendedSlotText() const
{
    return recommendedButton_->text();
}

void ReservationConfirmPage::refreshVehicles()
{
    const qint64 previous = vehicleComboBox_->currentData().toLongLong();
    vehicleComboBox_->blockSignals(true);
    vehicleComboBox_->clear();
    if (settings_ != nullptr) {
        for (const auto& vehicle : settings_->vehicles()) {
            const QString label = vehicle.isDefault
                ? tr("%1（默认）").arg(vehicle.plate)
                : vehicle.plate;
            vehicleComboBox_->addItem(label, vehicle.id);
        }
    }
    // 优先保持原选择；否则选中默认车辆。
    int index = vehicleComboBox_->findData(previous);
    if (index < 0 && settings_ != nullptr) {
        if (const auto* def = settings_->defaultVehicle(); def != nullptr) {
            index = vehicleComboBox_->findData(def->id);
        }
    }
    vehicleComboBox_->setCurrentIndex(index);
    vehicleComboBox_->blockSignals(false);
}

void ReservationConfirmPage::applyRecommendedSlot()
{
    // 模拟估算（ReservationService::recommendSlot）：出发准备 + 距离换算
    // 行驶时长后对齐 15 分钟刻度，时长取规格上限 45 分钟。
    // 真实地图接入点：腾讯路线规划/距离矩阵 API 就绪后仅需替换该估算来源。
    const auto slot = ReservationService::recommendSlot(
        distanceMeters_, QDateTime::currentDateTimeUtc());
    startEdit_->setDateTime(slot.startUtc.toLocalTime());
    endEdit_->setDateTime(slot.endUtc.toLocalTime());
    recommendedButton_->setText(
        tr("✨ 推荐 %1—%2 · 约 %3 分钟车程")
            .arg(slot.startUtc.toLocalTime().toString(QStringLiteral("HH:mm")),
                 slot.endUtc.toLocalTime().toString(QStringLiteral("HH:mm")))
            .arg(slot.travelMinutes));
    updateSlotValidity();
}

void ReservationConfirmPage::updateSlotValidity()
{
    if (submitting_) {
        return;
    }
    const int minutes = selectedMinutes();

    // 车辆前置条件：预约名额由车辆决定，无车辆则无法发起预约。
    if (vehicleComboBox_->count() == 0) {
        messageLabel_->setStyleSheet(QStringLiteral("color: #D48806;"));
        messageLabel_->setText(tr("⚠️ 请先在「设置 - 车辆管理」添加车辆，再发起预约"));
        messageLabel_->show();
        confirmButton_->setEnabled(false);
        feeLabel_->setText(tr("预估费用 ≈ ¥--"));
        return;
    }
    if (minutes <= 0) {
        messageLabel_->setStyleSheet(QStringLiteral("color: #E5484D;"));
        messageLabel_->setText(tr("⚠️ 结束时间必须晚于开始时间"));
        messageLabel_->show();
        confirmButton_->setEnabled(false);
    } else if (minutes > kMaxSlotMinutes) {
        // 规格约束：推荐时间段最大 45 分钟。
        messageLabel_->setStyleSheet(QStringLiteral("color: #E5484D;"));
        messageLabel_->setText(tr("⚠️ 预约时间段不能超过 %1 分钟，请缩短时段").arg(kMaxSlotMinutes));
        messageLabel_->show();
        confirmButton_->setEnabled(false);
    } else {
        messageLabel_->hide();
        confirmButton_->setEnabled(true);
    }

    const qint64 feeCents = station_.priceCentsPerKwh * qMax(0, minutes) / 60;
    feeLabel_->setText(tr("预估费用 ≈ ¥%1（¥%2/度 × %3 分钟）")
                           .arg(QString::number(feeCents / 100.0, 'f', 2))
                           .arg(QString::number(station_.priceCentsPerKwh / 100.0, 'f', 2))
                           .arg(qMax(0, minutes)));
}

void ReservationConfirmPage::resetSubmitButton()
{
    confirmButton_->setEnabled(true);
    confirmButton_->setText(tr("确认预约"));
}

void ReservationConfirmPage::handleSubmit()
{
    if (submitting_) {
        return;
    }
    if (service_ == nullptr) {
        messageLabel_->setStyleSheet(QStringLiteral("color: #E5484D;"));
        messageLabel_->setText(tr("⚠️ 预约服务未就绪，请稍后重试"));
        messageLabel_->show();
        return;
    }
    if (vehicleComboBox_->count() == 0) {
        return; // 无车辆：入口已置灰并提示，此处兜底
    }
    messageLabel_->hide();
    service_->submit(charger_, station_, startUtc(), endUtc(), selectedVehicleId(),
                     vehicleComboBox_->currentText(), distanceMeters_);
}

void ReservationConfirmPage::handleSubmitStarted(qint64 chargerId)
{
    if (chargerId != charger_.id) {
        return;
    }
    // loading 态：按钮禁用 + “提交中…”，防止重复提交。
    submitting_ = true;
    confirmButton_->setEnabled(false);
    confirmButton_->setText(tr("提交中…"));
}

void ReservationConfirmPage::handleSubmitSucceeded(
    const services::reservation::ReservationRecord& record)
{
    if (record.reservation.chargerId != charger_.id || !submitting_) {
        return;
    }
    submitting_ = false;
    // 预约成功：交给宿主弹“是否现在前往充电？”（导航页 / 预约订单页）。
    emit succeeded(record);
}

void ReservationConfirmPage::handleSubmitFailed(const QString& reason)
{
    if (!submitting_) {
        return;
    }
    // 失败：展示原因（桩被抢占/名额占用/参数非法/网络错误），
    // 停留在本页，按钮恢复可修改后重试。
    submitting_ = false;
    resetSubmitButton();
    messageLabel_->setStyleSheet(QStringLiteral("color: #E5484D;"));
    messageLabel_->setText(tr("⚠️ %1").arg(reason));
    messageLabel_->show();
    updateSlotValidity();
}

} // namespace charging::client::pages::station
