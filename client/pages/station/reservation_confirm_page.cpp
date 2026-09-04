#include "pages/station/reservation_confirm_page.h"

#include "charging/client/widgets/card.h"
#include "pages/station/platform_theme.h"
#include "services/reservation/reservation_service.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

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

    auto* durationRow = new QHBoxLayout();
    auto* durationCaption = new QLabel(tr("预约时长"), card);
    durationCaption->setProperty("role", QStringLiteral("secondary"));
    durationComboBox_ = new QComboBox(card);
    durationComboBox_->setObjectName(QStringLiteral("reservationDurationComboBox"));
    durationComboBox_->addItem(tr("30 分钟"), 30);
    durationComboBox_->addItem(tr("60 分钟"), 60);
    durationComboBox_->addItem(tr("90 分钟"), 90);
    durationComboBox_->addItem(tr("120 分钟"), 120);
    durationComboBox_->setCurrentIndex(1); // 默认 60 分钟
    durationRow->addWidget(durationCaption);
    durationRow->addStretch();
    durationRow->addWidget(durationComboBox_);
    body->addLayout(durationRow);

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
    connect(durationComboBox_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateEstimatedFee(); });
}

void ReservationConfirmPage::setService(
    services::reservation::ReservationService* service)
{
    if (service_ == service) {
        return;
    }
    service_ = service;
    if (service_ != nullptr) {
        connect(service_, &services::reservation::ReservationService::submitStarted, this,
                &ReservationConfirmPage::handleSubmitStarted);
        connect(service_, &services::reservation::ReservationService::submitSucceeded, this,
                &ReservationConfirmPage::handleSubmitSucceeded);
        connect(service_, &services::reservation::ReservationService::submitFailed, this,
                &ReservationConfirmPage::handleSubmitFailed);
    }
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

    // 复位表单与提示：每次进入都是一次全新预约。
    durationComboBox_->setCurrentIndex(1);
    messageLabel_->hide();
    submitting_ = false;
    confirmButton_->setEnabled(true);
    confirmButton_->setText(tr("确认预约"));
    updateEstimatedFee();
}

ReservationConfirmPage::PageState ReservationConfirmPage::pageState() const
{
    return submitting_ ? PageState::Submitting : PageState::Idle;
}

int ReservationConfirmPage::selectedMinutes() const
{
    return durationComboBox_->currentData().toInt();
}

QString ReservationConfirmPage::estimatedFeeText() const
{
    return feeLabel_->text();
}

QString ReservationConfirmPage::messageText() const
{
    return messageLabel_->text();
}

void ReservationConfirmPage::updateEstimatedFee()
{
    const qint64 feeCents = station_.priceCentsPerKwh * selectedMinutes() / 60;
    feeLabel_->setText(tr("预估费用 ≈ ¥%1（¥%2/度 × %3 分钟）")
                           .arg(QString::number(feeCents / 100.0, 'f', 2))
                           .arg(QString::number(station_.priceCentsPerKwh / 100.0, 'f', 2))
                           .arg(selectedMinutes()));
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
    messageLabel_->hide();
    service_->submit(charger_, station_, selectedMinutes(), distanceMeters_);
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
    // 预约成功：宿主据此刷新桩状态并自动路由至【预约订单】页面。
    emit confirmed(record);
}

void ReservationConfirmPage::handleSubmitFailed(const QString& reason)
{
    if (!submitting_) {
        return;
    }
    // 失败：展示原因（桩被抢占/已有未结束预约/参数非法/网络错误），
    // 停留在本页，按钮恢复可修改后重试。
    submitting_ = false;
    confirmButton_->setEnabled(true);
    confirmButton_->setText(tr("确认预约"));
    messageLabel_->setStyleSheet(QStringLiteral("color: #E5484D;"));
    messageLabel_->setText(tr("⚠️ %1").arg(reason));
    messageLabel_->show();
}

} // namespace charging::client::pages::station
