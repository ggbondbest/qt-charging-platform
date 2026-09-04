#include "pages/station/settings_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/status_tag.h"
#include "pages/station/platform_theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

using services::settings::SettingsService;
using services::settings::Vehicle;

// 页面局部样式：账号安全/车辆管理区块专属，仅本页生效，不改全局 QSS。
const char* kSettingsPageStyleSheet = R"(
QWidget#settingsPage {
    background: #F7F9FB;
}
QLabel#settingsPageTitle {
    color: #1F2937;
    font-size: 16px;
    font-weight: 700;
}
QLabel#settingsSectionTitle {
    color: #1F2937;
    font-size: 14px;
    font-weight: 700;
}
QPushButton#vehicleDeleteButton {
    background: #FFFFFF;
    color: #E5484D;
    border: 1px solid #E5484D;
    border-radius: 14px;
    padding: 4px 12px;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#vehicleDeleteButton:pressed {
    background: #FDEbec;
}
QPushButton#addVehicleButton {
    background: #E6F7F0;
    color: #00795A;
    border: 1px dashed #7FD4B8;
    border-radius: 14px;
    padding: 7px 16px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#vehicleEditButton, QPushButton#vehicleSetDefaultButton {
    background: #F4F6F8;
    color: #1F2937;
    border: 1px solid #D5DCE4;
    border-radius: 14px;
    padding: 4px 12px;
    font-size: 12px;
    font-weight: 600;
}
QLabel#settingsCaptionLabel {
    color: #9AA5B1;
    font-size: 11px;
}
QLabel#protectionSwitchHint {
    color: #D48806;
    font-size: 12px;
    font-weight: 600;
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

SettingsPage::SettingsPage(QWidget* parent) : QWidget(parent)
{
    installPlatformTheme();

    setObjectName(QStringLiteral("settingsPage"));
    setStyleSheet(QString::fromLatin1(kSettingsPageStyleSheet));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    auto* titleLabel = new QLabel(tr("设置"), this);
    titleLabel->setObjectName(QStringLiteral("settingsPageTitle"));
    rootLayout->addWidget(titleLabel);

    // 长内容滚动容器：鼠标滚轮上下滚动（规格通用要求）。
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rootLayout->addWidget(scroll, 1);

    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 8, 0);
    contentLayout->setSpacing(10);
    scroll->setWidget(content);

    contentLayout->addWidget(buildSecuritySection());
    contentLayout->addWidget(buildVehicleSection());
    contentLayout->addWidget(buildNotificationSection());
    contentLayout->addStretch();

    refresh();
}

void SettingsPage::setSettingsService(SettingsService* settings)
{
    if (settings_ == settings) {
        return;
    }
    settings_ = settings;
    if (settings_ != nullptr) {
        // 服务状态 → 页面实时联动（预约链路共用同一实例）。
        connect(settings_, &SettingsService::vehiclesChanged, this,
                &SettingsPage::refreshVehicleSection);
        connect(settings_, &SettingsService::protectionStateChanged, this,
                &SettingsPage::refreshSecuritySection);
        connect(settings_, &SettingsService::notificationsChanged, this,
                &SettingsPage::refreshNotificationSection);
    }
    refresh();
}

void SettingsPage::refresh()
{
    refreshSecuritySection();
    refreshVehicleSection();
    refreshNotificationSection();
}

// —— ① 账号安全 ——

QWidget* SettingsPage::buildSecuritySection()
{
    auto* card = new Card(this);
    card->setObjectName(QStringLiteral("settingsSecurityCard"));
    card->setProperty("isSettingsCard", true);
    auto* body = card->bodyLayout();

    auto* title = new QLabel(tr("🔐 账号安全"), card);
    title->setObjectName(QStringLiteral("settingsSectionTitle"));
    body->addWidget(title);

    auto* passwordRow = new QHBoxLayout();
    passwordStatusLabel_ = new QLabel(card);
    passwordStatusLabel_->setObjectName(QStringLiteral("protectionPasswordLabel"));
    passwordStatusLabel_->setProperty("role", QStringLiteral("secondary"));
    passwordButton_ = new QPushButton(tr("设置密码"), card);
    passwordButton_->setObjectName(QStringLiteral("setProtectionPasswordButton"));
    passwordButton_->setCursor(Qt::PointingHandCursor);
    connect(passwordButton_, &QPushButton::clicked, this, &SettingsPage::openPasswordDialog);
    passwordRow->addWidget(passwordStatusLabel_);
    passwordRow->addStretch();
    passwordRow->addWidget(passwordButton_);
    body->addLayout(passwordRow);

    protectionSwitch_ = new QCheckBox(tr("开启二级保护密码验证"), card);
    protectionSwitch_->setObjectName(QStringLiteral("protectionSwitch"));
    protectionHintLabel_ = new QLabel(card);
    protectionHintLabel_->setObjectName(QStringLiteral("protectionSwitchHint"));
    protectionHintLabel_->setWordWrap(true);
    body->addWidget(protectionSwitch_);
    body->addWidget(protectionHintLabel_);

    // 开关兜底：Service 在未设置密码时拒绝开启（返回 false），UI 复位勾选。
    connect(protectionSwitch_, &QCheckBox::toggled, this, [this](bool on) {
        if (settings_ == nullptr) {
            return;
        }
        if (!settings_->setProtectionEnabled(on)) {
            QSignalBlocker blocker(protectionSwitch_);
            protectionSwitch_->setChecked(settings_->protectionEnabled());
            refreshSecuritySection();
        }
    });
    return card;
}

void SettingsPage::refreshSecuritySection()
{
    if (settings_ == nullptr) {
        return;
    }
    const bool hasPassword = settings_->hasProtectionPassword();
    passwordStatusLabel_->setText(hasPassword ? tr("二级保护密码：已设置") : tr("二级保护密码：未设置"));
    passwordButton_->setText(hasPassword ? tr("修改密码") : tr("设置密码"));
    {
        QSignalBlocker blocker(protectionSwitch_);
        protectionSwitch_->setEnabled(hasPassword);
        protectionSwitch_->setChecked(hasPassword && settings_->protectionEnabled());
    }
    protectionHintLabel_->setText(hasPassword
                                      ? (protectionSwitch_->isChecked()
                                             ? tr("关键操作（预约/取消）将要求输入二级密码")
                                             : tr("当前未开启，关键操作不做二次验证"))
                                      : tr("未设置二级保护密码，开关暂不可用——请先点击上方「设置密码」"));
}

// —— ② 车辆管理 ——

QWidget* SettingsPage::buildVehicleSection()
{
    auto* card = new Card(this);
    card->setObjectName(QStringLiteral("settingsVehicleCard"));
    card->setProperty("isSettingsCard", true);
    auto* body = card->bodyLayout();

    auto* title = new QLabel(tr("🚗 车辆管理"), card);
    title->setObjectName(QStringLiteral("settingsSectionTitle"));
    body->addWidget(title);

    slotsCaptionLabel_ = new QLabel(card);
    slotsCaptionLabel_->setObjectName(QStringLiteral("settingsCaptionLabel"));
    slotsCaptionLabel_->setWordWrap(true);
    body->addWidget(slotsCaptionLabel_);

    vehiclesEmptyLabel_ = new QLabel(tr("暂无车辆，预约需先添加车辆"), card);
    vehiclesEmptyLabel_->setObjectName(QStringLiteral("vehiclesEmptyLabel"));
    vehiclesEmptyLabel_->setProperty("role", QStringLiteral("secondary"));
    vehiclesEmptyLabel_->setAlignment(Qt::AlignCenter);
    body->addWidget(vehiclesEmptyLabel_);

    // 车辆卡片列表容器（每次刷新重建）。
    vehiclesHost_ = new QWidget(card);
    vehiclesHost_->setObjectName(QStringLiteral("vehiclesListHost"));
    vehiclesLayout_ = new QVBoxLayout(vehiclesHost_);
    vehiclesLayout_->setContentsMargins(0, 0, 0, 0);
    vehiclesLayout_->setSpacing(8);
    body->addWidget(vehiclesHost_);

    auto* addButton = new QPushButton(tr("＋ 添加车辆"), card);
    addButton->setObjectName(QStringLiteral("addVehicleButton"));
    addButton->setCursor(Qt::PointingHandCursor);
    connect(addButton, &QPushButton::clicked, this, [this]() { openVehicleDialog(0); });
    body->addWidget(addButton, 0, Qt::AlignLeft);
    return card;
}

void SettingsPage::refreshVehicleSection()
{
    clearLayoutItems(vehiclesLayout_);
    const int count = settings_ != nullptr ? settings_->vehicleCount() : 0;
    if (settings_ != nullptr) {
        for (const auto& vehicle : settings_->vehicles()) {
            vehiclesLayout_->addWidget(createVehicleCard(vehicle));
        }
    }
    vehiclesEmptyLabel_->setVisible(count == 0);
    // 业务联动说明：车辆数 = 可预约时段名额。
    slotsCaptionLabel_->setText(count == 0
                                    ? tr("当前 0 辆车 → 无法发起预约；添加车辆后即可预约")
                                    : tr("当前 %1 辆车 → 最多可同时持有 %1 个有效预约时段（每辆 1 个）")
                                          .arg(count));
}

QWidget* SettingsPage::createVehicleCard(const Vehicle& vehicle)
{
    auto* card = new ClickableCard(vehiclesHost_);
    card->setObjectName(QStringLiteral("vehicleCard"));
    card->setProperty("isVehicleCard", true);
    card->setProperty("vehicleId", vehicle.id);
    card->setAccessibleName(vehicle.plate);
    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    auto* plateLabel = new QLabel(vehicle.plate, card);
    plateLabel->setProperty("role", QStringLiteral("sectionTitle"));
    titleRow->addWidget(plateLabel);
    titleRow->addStretch();
    if (vehicle.isDefault) {
        auto* tag = new StatusTag(tr("默认"), StatusTag::Tone::Info, card);
        tag->setObjectName(QStringLiteral("vehicleDefaultTag"));
        titleRow->addWidget(tag);
    }
    body->addLayout(titleRow);

    const bool fast = vehicle.connectorType == charging::model::ChargerType::Fast;
    auto* specLabel = new QLabel(
        tr("%1 · 接口：%2 · 电池：%3 kWh")
            .arg(vehicle.brandModel.isEmpty() ? tr("未填写品牌型号") : vehicle.brandModel,
                 fast ? tr("快充（直流）") : tr("慢充（交流）"), QString::number(vehicle.batteryKwh)),
        card);
    specLabel->setProperty("role", QStringLiteral("secondary"));
    specLabel->setWordWrap(true);
    body->addWidget(specLabel);

    auto* actionRow = new QHBoxLayout();
    actionRow->addStretch();
    if (!vehicle.isDefault) {
        auto* defaultButton = new QPushButton(tr("设为默认"), card);
        defaultButton->setObjectName(QStringLiteral("vehicleSetDefaultButton"));
        defaultButton->setCursor(Qt::PointingHandCursor);
        const qint64 id = vehicle.id;
        connect(defaultButton, &QPushButton::clicked, this,
                [this, id]() { settings_->setDefaultVehicle(id); });
        actionRow->addWidget(defaultButton);
    }
    auto* editButton = new QPushButton(tr("编辑"), card);
    editButton->setObjectName(QStringLiteral("vehicleEditButton"));
    editButton->setCursor(Qt::PointingHandCursor);
    const qint64 editId = vehicle.id;
    connect(editButton, &QPushButton::clicked, this,
            [this, editId]() { openVehicleDialog(editId); });
    actionRow->addWidget(editButton);
    auto* deleteButton = new QPushButton(tr("删除"), card);
    deleteButton->setObjectName(QStringLiteral("vehicleDeleteButton"));
    deleteButton->setCursor(Qt::PointingHandCursor);
    connect(deleteButton, &QPushButton::clicked, this, [this, id = vehicle.id]() {
        settings_->removeVehicle(id);
    });
    actionRow->addWidget(deleteButton);
    body->addLayout(actionRow);

    return card;
}

void SettingsPage::openVehicleDialog(qint64 vehicleId)
{
    if (settings_ == nullptr || vehicleDialog_ != nullptr) {
        return;
    }
    const Vehicle existing
        = vehicleId > 0 && settings_->vehicle(vehicleId) != nullptr
            ? *settings_->vehicle(vehicleId)
            : Vehicle{};

    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("vehicleDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(vehicleId > 0 ? tr("编辑车辆") : tr("添加车辆"));
    dialog->resize(360, 360);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    const auto addField = [&](const QString& caption, QWidget* field) {
        auto* row = new QHBoxLayout();
        auto* label = new QLabel(caption, dialog);
        label->setProperty("role", QStringLiteral("secondary"));
        row->addWidget(label);
        row->addStretch();
        row->addWidget(field);
        layout->addLayout(row);
    };

    auto* plateEdit = new QLineEdit(existing.plate, dialog);
    plateEdit->setObjectName(QStringLiteral("vehiclePlateEdit"));
    plateEdit->setPlaceholderText(tr("如：粤B·DA1234"));
    addField(tr("车牌号码"), plateEdit);

    auto* brandEdit = new QLineEdit(existing.brandModel, dialog);
    brandEdit->setObjectName(QStringLiteral("vehicleBrandEdit"));
    brandEdit->setPlaceholderText(tr("如：比亚迪 汉 EV"));
    addField(tr("品牌型号"), brandEdit);

    auto* batterySpin = new QSpinBox(dialog);
    batterySpin->setObjectName(QStringLiteral("vehicleBatterySpin"));
    batterySpin->setRange(10, 200);
    batterySpin->setSuffix(QStringLiteral(" kWh"));
    batterySpin->setValue(existing.batteryKwh > 0 ? existing.batteryKwh : 60);
    addField(tr("电池容量"), batterySpin);

    auto* connectorRow = new QHBoxLayout();
    auto* fastRadio = new QRadioButton(tr("快充（直流）"), dialog);
    fastRadio->setObjectName(QStringLiteral("vehicleFastConnectorRadio"));
    fastRadio->setChecked(existing.connectorType == charging::model::ChargerType::Fast);
    auto* slowRadio = new QRadioButton(tr("慢充（交流）"), dialog);
    slowRadio->setObjectName(QStringLiteral("vehicleSlowConnectorRadio"));
    slowRadio->setChecked(existing.connectorType == charging::model::ChargerType::Slow);
    auto* connectorGroup = new QButtonGroup(dialog);
    connectorGroup->addButton(fastRadio);
    connectorGroup->addButton(slowRadio);
    connectorRow->addWidget(new QLabel(tr("接口类型"), dialog));
    connectorRow->addStretch();
    connectorRow->addWidget(fastRadio);
    connectorRow->addWidget(slowRadio);
    layout->addLayout(connectorRow);

    auto* defaultCheck = new QCheckBox(tr("设为默认车辆（预约时默认选用）"), dialog);
    defaultCheck->setObjectName(QStringLiteral("vehicleDefaultCheck"));
    defaultCheck->setChecked(existing.isDefault || settings_->vehicleCount() == 0);
    layout->addWidget(defaultCheck);

    auto* messageLabel = new QLabel(dialog);
    messageLabel->setObjectName(QStringLiteral("vehicleDialogMessage"));
    messageLabel->setWordWrap(true);
    messageLabel->setStyleSheet(QStringLiteral("color: #E5484D; font-size: 12px;"));
    messageLabel->hide();
    layout->addWidget(messageLabel);

    auto* buttonRow = new QHBoxLayout();
    auto* cancelButton = new QPushButton(tr("取消"), dialog);
    cancelButton->setObjectName(QStringLiteral("vehicleCancelButton"));
    auto* saveButton = new QPushButton(vehicleId > 0 ? tr("保存修改") : tr("保存车辆"), dialog);
    saveButton->setObjectName(QStringLiteral("vehicleSaveButton"));
    saveButton->setCursor(Qt::PointingHandCursor);
    buttonRow->addStretch();
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(saveButton);
    layout->addLayout(buttonRow);

    connect(cancelButton, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, dialog, [this, dialog, plateEdit, brandEdit,
                                                        batterySpin, fastRadio, defaultCheck,
                                                        messageLabel, vehicleId, existing]() {
        const QString plate = plateEdit->text().trimmed();
        if (plate.isEmpty()) {
            messageLabel->setText(tr("请填写车牌号码"));
            messageLabel->show();
            return;
        }
        Vehicle vehicle = existing;
        vehicle.plate = plate;
        vehicle.brandModel = brandEdit->text().trimmed();
        vehicle.batteryKwh = batterySpin->value();
        vehicle.connectorType
            = fastRadio->isChecked() ? charging::model::ChargerType::Fast
                                     : charging::model::ChargerType::Slow;
        vehicle.isDefault = defaultCheck->isChecked();
        if (vehicleId > 0) {
            settings_->updateVehicle(vehicle);
        } else {
            settings_->addVehicle(vehicle); // 首台自动默认；Service 规整标记
        }
        dialog->accept();
    });

    vehicleDialog_ = dialog;
    dialog->show(); // 非模态：与预约详情弹窗一致，宿主事件循环内可交互
}

// —— ③ 通知与提醒 ——

QWidget* SettingsPage::buildNotificationSection()
{
    auto* card = new Card(this);
    card->setObjectName(QStringLiteral("settingsNotificationCard"));
    card->setProperty("isSettingsCard", true);
    auto* body = card->bodyLayout();

    auto* title = new QLabel(tr("🔔 通知与提醒"), card);
    title->setObjectName(QStringLiteral("settingsSectionTitle"));
    body->addWidget(title);

    const auto addSwitch = [&](const QString& text, const QString& objectName,
                               QCheckBox*& field) {
        field = new QCheckBox(text, card);
        field->setObjectName(objectName);
        body->addWidget(field);
    };
    addSwitch(tr("预约到期提醒"), QStringLiteral("expiryReminderSwitch"), expirySwitch_);
    addSwitch(tr("预约成功通知"), QStringLiteral("reservationSuccessSwitch"), successSwitch_);
    addSwitch(tr("预约取消通知"), QStringLiteral("reservationCancelSwitch"), cancelSwitch_);

    auto* caption = new QLabel(tr("开关本地保存，重进页面后仍然生效"), card);
    caption->setObjectName(QStringLiteral("settingsCaptionLabel"));
    body->addWidget(caption);

    // 切换即持久化（QSettings 经 SettingsService 写入）。
    connect(expirySwitch_, &QCheckBox::toggled, this, [this](bool on) {
        if (settings_ != nullptr) {
            settings_->setNotificationEnabled(SettingsService::Notification::ReservationExpiryReminder,
                                              on);
        }
    });
    connect(successSwitch_, &QCheckBox::toggled, this, [this](bool on) {
        if (settings_ != nullptr) {
            settings_->setNotificationEnabled(SettingsService::Notification::ReservationSuccessNotice,
                                              on);
        }
    });
    connect(cancelSwitch_, &QCheckBox::toggled, this, [this](bool on) {
        if (settings_ != nullptr) {
            settings_->setNotificationEnabled(SettingsService::Notification::ReservationCancelNotice,
                                              on);
        }
    });
    return card;
}

void SettingsPage::refreshNotificationSection()
{
    if (settings_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker1(expirySwitch_);
    const QSignalBlocker blocker2(successSwitch_);
    const QSignalBlocker blocker3(cancelSwitch_);
    expirySwitch_->setChecked(settings_->notificationEnabled(
        SettingsService::Notification::ReservationExpiryReminder));
    successSwitch_->setChecked(settings_->notificationEnabled(
        SettingsService::Notification::ReservationSuccessNotice));
    cancelSwitch_->setChecked(settings_->notificationEnabled(
        SettingsService::Notification::ReservationCancelNotice));
}

// —— 二级保护密码对话框 ——

void SettingsPage::openPasswordDialog()
{
    if (settings_ == nullptr || passwordDialog_ != nullptr) {
        return;
    }
    const bool changing = settings_->hasProtectionPassword();

    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("passwordDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(changing ? tr("修改二级保护密码") : tr("设置二级保护密码"));
    dialog->resize(340, 300);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* hint = new QLabel(tr("密码至少 4 位，仅保存哈希值（不存明文）"), dialog);
    hint->setObjectName(QStringLiteral("settingsCaptionLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    QLineEdit* currentEdit = nullptr;
    if (changing) {
        currentEdit = new QLineEdit(dialog);
        currentEdit->setObjectName(QStringLiteral("currentPasswordEdit"));
        currentEdit->setEchoMode(QLineEdit::Password);
        currentEdit->setPlaceholderText(tr("当前密码"));
        layout->addWidget(new QLabel(tr("当前密码"), dialog));
        layout->addWidget(currentEdit);
    }
    auto* newEdit = new QLineEdit(dialog);
    newEdit->setObjectName(QStringLiteral("newPasswordEdit"));
    newEdit->setEchoMode(QLineEdit::Password);
    newEdit->setPlaceholderText(tr("新密码（≥ 4 位）"));
    layout->addWidget(new QLabel(tr("新密码"), dialog));
    layout->addWidget(newEdit);

    auto* confirmEdit = new QLineEdit(dialog);
    confirmEdit->setObjectName(QStringLiteral("confirmPasswordEdit"));
    confirmEdit->setEchoMode(QLineEdit::Password);
    confirmEdit->setPlaceholderText(tr("再次输入新密码"));
    layout->addWidget(new QLabel(tr("确认密码"), dialog));
    layout->addWidget(confirmEdit);

    auto* messageLabel = new QLabel(dialog);
    messageLabel->setObjectName(QStringLiteral("passwordDialogMessage"));
    messageLabel->setWordWrap(true);
    messageLabel->setStyleSheet(QStringLiteral("color: #E5484D; font-size: 12px;"));
    messageLabel->hide();
    layout->addWidget(messageLabel);
    layout->addStretch();

    auto* buttonRow = new QHBoxLayout();
    auto* cancelButton = new QPushButton(tr("取消"), dialog);
    cancelButton->setObjectName(QStringLiteral("passwordCancelButton"));
    auto* saveButton = new QPushButton(tr("保存密码"), dialog);
    saveButton->setObjectName(QStringLiteral("passwordSaveButton"));
    saveButton->setCursor(Qt::PointingHandCursor);
    buttonRow->addStretch();
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(saveButton);
    layout->addLayout(buttonRow);

    connect(cancelButton, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(saveButton, &QPushButton::clicked, dialog,
            [this, dialog, currentEdit, newEdit, confirmEdit, messageLabel, changing]() {
                const auto fail = [&](const QString& text) {
                    messageLabel->setText(text);
                    messageLabel->show();
                };
                if (changing && !settings_->verifyProtectionPassword(currentEdit->text())) {
                    fail(tr("当前密码不正确"));
                    return;
                }
                if (newEdit->text().size() < 4) {
                    fail(tr("密码长度至少 4 位"));
                    return;
                }
                if (newEdit->text() != confirmEdit->text()) {
                    fail(tr("两次输入的密码不一致"));
                    return;
                }
                settings_->setProtectionPassword(newEdit->text()); // SHA-256 哈希落盘
                dialog->accept();
            });

    passwordDialog_ = dialog;
    dialog->show(); // 非模态：与预约详情弹窗一致，宿主事件循环内可交互
}

// —— 测试探针 ——

int SettingsPage::vehicleCardCount() const
{
    int count = 0;
    for (int i = 0; i < vehiclesLayout_->count(); ++i) {
        const auto* item = vehiclesLayout_->itemAt(i);
        if (item != nullptr && item->widget() != nullptr
            && item->widget()->property("isVehicleCard").toBool()) {
            ++count;
        }
    }
    return count;
}

bool SettingsPage::protectionSwitchEnabled() const
{
    return protectionSwitch_->isEnabled();
}

bool SettingsPage::protectionSwitchChecked() const
{
    return protectionSwitch_->isChecked();
}

QString SettingsPage::passwordStatusText() const
{
    return passwordStatusLabel_->text();
}

QString SettingsPage::slotsCaptionText() const
{
    return slotsCaptionLabel_->text();
}

} // namespace charging::client::pages::station
