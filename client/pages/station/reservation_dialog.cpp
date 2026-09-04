#include "pages/station/reservation_dialog.h"

#include "charging/client/widgets/card.h"
#include "services/reservation/reservation_service.h"
#include "pages/station/platform_theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

// 弹窗局部样式：电动绿 token 与全局主题一致，仅本组件生效。
const char* kReservationDialogStyleSheet = R"(
QDialog#reservationDialog {
    background: #F7F9FB;
}
QLabel#reservationDialogTitle {
    color: #1F2937;
    font-size: 16px;
    font-weight: 700;
}
QPushButton#reservationSubmitButton {
    background: #00B578;
    color: #FFFFFF;
    border: none;
    border-radius: 16px;
    padding: 7px 18px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#reservationSubmitButton:pressed {
    background: #009A66;
}
QPushButton#reservationSubmitButton:disabled {
    background: #B9C4CF;
}
QPushButton#reservationCloseButton {
    background: #F4F6F8;
    color: #1F2937;
    border: 1px solid #D5DCE4;
    border-radius: 16px;
    padding: 7px 18px;
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

ReservationDialog::ReservationDialog(const charging::model::Station& station,
                                     const charging::model::Charger& charger, QWidget* parent)
    : QDialog(parent), station_(station), charger_(charger)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("reservationDialog"));
    setStyleSheet(QString::fromLatin1(kReservationDialogStyleSheet));
    setWindowTitle(tr("预约充电桩"));
    setModal(true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 14, 16, 14);
    rootLayout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("预约充电桩"), this);
    titleLabel->setObjectName(QStringLiteral("reservationDialogTitle"));
    rootLayout->addWidget(titleLabel);

    auto* card = new Card(this);
    auto* body = card->bodyLayout();

    auto addRow = [&](const QString& caption, const QString& value, const QString& objectName) {
        auto* row = new QHBoxLayout();
        auto* captionLabel = new QLabel(caption, card);
        captionLabel->setProperty("role", QStringLiteral("secondary"));
        auto* valueLabel = new QLabel(value, card);
        valueLabel->setObjectName(objectName);
        valueLabel->setProperty("role", QStringLiteral("sectionTitle"));
        row->addWidget(captionLabel);
        row->addStretch();
        row->addWidget(valueLabel);
        body->addLayout(row);
    };

    addRow(tr("站点名称"), station_.name, QStringLiteral("reservationStationNameLabel"));
    const bool fast = charger_.type == charging::model::ChargerType::Fast;
    addRow(tr("充电桩编号"),
           QStringLiteral("%1（%2 %3kW）")
               .arg(charger_.code)
               .arg(fast ? tr("快充") : tr("慢充"))
               .arg(charger_.powerWatts / 1000),
           QStringLiteral("reservationChargerCodeLabel"));

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
    body->addWidget(feeLabel_);
    rootLayout->addWidget(card);

    messageLabel_ = new QLabel(this);
    messageLabel_->setObjectName(QStringLiteral("reservationMessageLabel"));
    messageLabel_->setWordWrap(true);
    messageLabel_->hide();
    rootLayout->addWidget(messageLabel_);

    auto* buttonRow = new QHBoxLayout();
    auto* closeButton = new QPushButton(tr("关闭"), this);
    closeButton->setObjectName(QStringLiteral("reservationCloseButton"));
    closeButton->setCursor(Qt::PointingHandCursor);
    submitButton_ = new QPushButton(tr("确认预约"), this);
    submitButton_->setObjectName(QStringLiteral("reservationSubmitButton"));
    submitButton_->setCursor(Qt::PointingHandCursor);
    buttonRow->addWidget(closeButton);
    buttonRow->addStretch();
    buttonRow->addWidget(submitButton_);
    rootLayout->addLayout(buttonRow);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(submitButton_, &QPushButton::clicked, this, &ReservationDialog::handleSubmit);
    connect(durationComboBox_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateEstimatedFee(); });

    updateEstimatedFee();
    resize(360, 300);
}

void ReservationDialog::setService(services::reservation::ReservationService* service)
{
    if (service_ == service) {
        return;
    }
    service_ = service;
    if (service_ != nullptr) {
        connect(service_, &services::reservation::ReservationService::submitStarted, this,
                &ReservationDialog::handleSubmitStarted);
        connect(service_, &services::reservation::ReservationService::submitSucceeded, this,
                &ReservationDialog::handleSubmitSucceeded);
        connect(service_, &services::reservation::ReservationService::submitFailed, this,
                &ReservationDialog::handleSubmitFailed);
    }
}

int ReservationDialog::selectedMinutes() const
{
    return durationComboBox_->currentData().toInt();
}

QString ReservationDialog::estimatedFeeText() const
{
    return feeLabel_->text();
}

QString ReservationDialog::messageText() const
{
    return messageLabel_->text();
}

void ReservationDialog::updateEstimatedFee()
{
    const qint64 feeCents = station_.priceCentsPerKwh * selectedMinutes() / 60;
    feeLabel_->setText(tr("预估费用 ≈ ¥%1（¥%2/度 × %3 分钟）")
                           .arg(QString::number(feeCents / 100.0, 'f', 2))
                           .arg(QString::number(station_.priceCentsPerKwh / 100.0, 'f', 2))
                           .arg(selectedMinutes()));
}

void ReservationDialog::handleSubmit()
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
    service_->submit(charger_, station_, selectedMinutes());
}

void ReservationDialog::handleSubmitStarted(qint64 chargerId)
{
    if (chargerId != charger_.id) {
        return;
    }
    // loading 态：按钮禁用 + “提交中…”，防止重复提交。
    submitting_ = true;
    submitButton_->setEnabled(false);
    submitButton_->setText(tr("提交中…"));
}

void ReservationDialog::handleSubmitSucceeded(
    const services::reservation::ReservationRecord& record)
{
    if (record.reservation.chargerId != charger_.id || !submitting_) {
        return;
    }
    submitting_ = false;
    messageLabel_->setStyleSheet(QStringLiteral("color: #00A76D;"));
    messageLabel_->setText(tr("✅ 预约成功！桩位已为你保留，弹窗即将关闭。"));
    messageLabel_->show();
    emit reserved(record.reservation.chargerId);
    // 短暂停留展示成功提示后自动关闭（宿主刷新桩状态）。
    QTimer::singleShot(900, this, [this]() {
        if (isVisible()) {
            accept();
        }
    });
}

void ReservationDialog::handleSubmitFailed(const QString& reason)
{
    if (!submitting_) {
        return;
    }
    // 失败：展示原因（桩被抢占/预约冲突/参数非法/网络错误），可重试或关闭。
    submitting_ = false;
    submitButton_->setEnabled(true);
    submitButton_->setText(tr("确认预约"));
    messageLabel_->setStyleSheet(QStringLiteral("color: #E5484D;"));
    messageLabel_->setText(tr("⚠️ %1").arg(reason));
    messageLabel_->show();
}

} // namespace charging::client::pages::station
