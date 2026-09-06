#include "pages/station/station_filter_dialog.h"

#include <algorithm>
#include <utility>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

using services::station::StationFilterCriteria;
using services::station::station_filter::kDistanceOptionsKm;

// 弹窗局部样式：token 与全局规范同源（client_platform.qss），仅本弹窗生效。
// 页面级样式不向 QDialog 级联，故在此自带一份（同设置页车辆弹窗口径）。
const char* kFilterDialogStyleSheet = R"(
QDialog#stationFilterDialog {
    background: #FFFFFF;
}
QLabel#stationFilterDialogTitle {
    color: #1F2937;
    font-size: 16px;
    font-weight: 700;
}
QLabel#stationFilterGroupTitle {
    color: #1F2937;
    font-size: 13px;
    font-weight: 600;
}
QLabel#stationFilterGroupHint {
    color: #9CA3AF;
    font-size: 11px;
}
QPushButton[filterChip="true"] {
    background: #F4F6F8;
    color: #6B7280;
    border: 1px solid #D5DCE4;
    border-radius: 14px;
    padding: 6px 14px;
    font-size: 12px;
}
QPushButton[filterChip="true"]:checked {
    background: #EAF9F2;
    color: #00A76D;
    border: 1px solid #00B578;
    font-weight: 600;
}
QPushButton#stationFilterResetButton {
    background: #F4F6F8;
    color: #6B7280;
    border: 1px solid #D5DCE4;
    border-radius: 12px;
    padding: 9px 0px;
    font-size: 14px;
}
QPushButton#stationFilterApplyButton {
    background: #00B578;
    color: #FFFFFF;
    border: none;
    border-radius: 12px;
    padding: 9px 0px;
    font-size: 14px;
    font-weight: 700;
}
QPushButton#stationFilterApplyButton:hover {
    background: #00A76D;
}
)";

constexpr int kDialogWidth = 460;
constexpr int kDialogHeight = 620;

} // namespace

QVector<StationFilterDialog::GroupSpec> StationFilterDialog::groupSpecs()
{
    namespace sf = services::station::station_filter;
    QStringList distanceLabels;
    for (const int km : sf::kDistanceOptionsKm) {
        distanceLabels << QStringLiteral("%1公里").arg(km);
    }
    return {
        {QStringLiteral("distance"), QStringLiteral("距离"), distanceLabels, true},
        {QStringLiteral("status"), QStringLiteral("运营状态"), sf::operatingStatusOptions(), false},
        {QStringLiteral("operator"), QStringLiteral("运营商"), sf::operatorOptions(), false},
        {QStringLiteral("access"), QStringLiteral("电站类型"), sf::accessTypeOptions(), false},
        {QStringLiteral("parking"), QStringLiteral("停车费"), sf::parkingFeeOptions(), false},
        {QStringLiteral("feature"), QStringLiteral("特色功能"), sf::featureOptions(), false},
        {QStringLiteral("chargerType"), QStringLiteral("充电桩类型"), sf::chargerTypeOptions(), false},
        {QStringLiteral("voltage"), QStringLiteral("充电桩电压"), sf::voltageBandOptions(), false},
    };
}

StationFilterDialog::StationFilterDialog(const StationFilterCriteria& initial,
                                         QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("stationFilterDialog"));
    setWindowTitle(tr("高级筛选"));
    setModal(false); // 非模态：与既有弹窗先例一致，列表可边看边调
    setAttribute(Qt::WA_DeleteOnClose);
    setFixedWidth(kDialogWidth);
    setFixedHeight(kDialogHeight);
    setStyleSheet(QString::fromLatin1(kFilterDialogStyleSheet));
    // accept()/reject() 只隐藏不销毁（WA_DeleteOnClose 挂在 closeEvent 上），
    // 统一经 finished→close 走 deleteLater，调用方 QPointer 去重随之复位。
    connect(this, &QDialog::finished, this, [this](int) { close(); });

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 16, 20, 16);
    rootLayout->setSpacing(10);

    auto* title = new QLabel(tr("高级筛选"), this);
    title->setObjectName(QStringLiteral("stationFilterDialogTitle"));
    rootLayout->addWidget(title);

    // 8 组条件可整体滚动（长内容 + 鼠标滚轮，规格通用要求）。
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("stationFilterScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* groups = new QVBoxLayout(content);
    groups->setContentsMargins(0, 0, 8, 0);
    groups->setSpacing(14);
    scroll->setWidget(content);
    contentLayout_ = groups;

    const QVector<GroupSpec> specs = groupSpecs();
    for (const GroupSpec& spec : specs) {
        auto* groupTitle = new QLabel(spec.title, content);
        groupTitle->setObjectName(QStringLiteral("stationFilterGroupTitle"));
        groups->addWidget(groupTitle);

        auto* chipsRow = new QHBoxLayout();
        chipsRow->setContentsMargins(0, 0, 0, 0);
        chipsRow->setSpacing(8);
        for (int i = 0; i < spec.options.size(); ++i) {
            auto* chip = new QPushButton(spec.options.at(i), content);
            chip->setObjectName(QStringLiteral("stationFilterChip_%1_%2")
                                    .arg(spec.key)
                                    .arg(i));
            chip->setCheckable(true);
            chip->setProperty("filterChip", true); // QSS 属性选择器
            chip->setProperty("filterGroup", spec.key);
            chip->setProperty("filterIndex", i);
            chip->setCursor(Qt::PointingHandCursor);
            connect(chip, &QPushButton::clicked, this,
                    [this, chip]() { onChipClicked(chip); });
            chipsRow->addWidget(chip);
        }
        chipsRow->addStretch();
        groups->addLayout(chipsRow);
    }
    groups->addStretch();
    rootLayout->addWidget(scroll, 1);

    // 底部：重置（清空勾选，不自动下发）+ 确定（下发过滤并关闭）。
    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(12);
    auto* resetButton = new QPushButton(tr("重置"), this);
    resetButton->setObjectName(QStringLiteral("stationFilterResetButton"));
    resetButton->setCursor(Qt::PointingHandCursor);
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        // 重置：清空全部勾选回到“未限制”态；不自动下发（规格：确定才过滤）。
        for (const GroupSpec& spec : groupSpecs()) {
            for (QPushButton* chip : chipsOfGroup(spec.key)) {
                chip->setChecked(false);
            }
        }
    });
    auto* applyButton = new QPushButton(tr("确定"), this);
    applyButton->setObjectName(QStringLiteral("stationFilterApplyButton"));
    applyButton->setCursor(Qt::PointingHandCursor);
    connect(applyButton, &QPushButton::clicked, this, [this]() {
        emit applied(currentCriteria());
        accept();
    });
    buttonRow->addWidget(resetButton);
    buttonRow->addWidget(applyButton);
    rootLayout->addLayout(buttonRow);

    // 初始回显（收藏夹页二次打开时保留已生效条件）。
    const QList<std::pair<QString, QStringList>> initialSelections = {
        {QStringLiteral("status"), initial.statuses},
        {QStringLiteral("operator"), initial.operators},
        {QStringLiteral("access"), initial.accessTypes},
        {QStringLiteral("parking"), initial.parkingFees},
        {QStringLiteral("feature"), initial.features},
        {QStringLiteral("chargerType"), initial.chargerTypes},
        {QStringLiteral("voltage"), initial.voltageBands},
    };
    for (const auto& [key, values] : initialSelections) {
        setGroupSelectionForTesting(key, values); // 同一套归属逻辑，避免双实现
    }
    if (initial.maxDistanceKm > 0) {
        for (int i = 0; i < 4; ++i) {
            if (kDistanceOptionsKm[i] == initial.maxDistanceKm) {
                const QVector<QPushButton*> chips = chipsOfGroup(QStringLiteral("distance"));
                if (i < chips.size()) {
                    chips.at(i)->setChecked(true);
                }
                break;
            }
        }
    }
}

void StationFilterDialog::onChipClicked(QPushButton* chip)
{
    const bool exclusive = chip->property("filterGroup").toString() == QStringLiteral("distance");
    if (!exclusive) {
        return; // 多选组交给 QPushButton 自带 checked 翻转
    }
    // 单选组：点选另一项自动取消前项；再点自己 = 取消（回到“不限距离”）。
    // clicked 信号在 checked 状态翻转后发出，此处按目标态收敛。
    const bool nowChecked = chip->isChecked();
    for (QPushButton* sibling : chipsOfGroup(QStringLiteral("distance"))) {
        if (sibling != chip) {
            sibling->setChecked(false);
        }
    }
    chip->setChecked(nowChecked);
}

QVector<QPushButton*> StationFilterDialog::chipsOfGroup(const QString& groupKey) const
{
    QVector<QPushButton*> chips;
    const auto buttons = findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->property("filterChip").toBool()
            && button->property("filterGroup").toString() == groupKey) {
            chips.append(button);
        }
    }
    // findChildren 顺序即加入顺序，此处再按 filterIndex 稳定排序以防万一。
    std::sort(chips.begin(), chips.end(), [](const QPushButton* a, const QPushButton* b) {
        return a->property("filterIndex").toInt() < b->property("filterIndex").toInt();
    });
    return chips;
}

int StationFilterDialog::checkedCountForGroup(const QString& groupKey) const
{
    int count = 0;
    for (const QPushButton* chip : chipsOfGroup(groupKey)) {
        if (chip->isChecked()) {
            ++count;
        }
    }
    return count;
}

void StationFilterDialog::setGroupSelectionForTesting(const QString& groupKey,
                                                      const QStringList& options)
{
    for (QPushButton* chip : chipsOfGroup(groupKey)) {
        chip->setChecked(options.contains(chip->text()));
    }
}

StationFilterCriteria StationFilterDialog::currentCriteria() const
{
    StationFilterCriteria criteria;
    for (const GroupSpec& spec : groupSpecs()) {
        for (const QPushButton* chip : chipsOfGroup(spec.key)) {
            if (!chip->isChecked()) {
                continue;
            }
            const int index = chip->property("filterIndex").toInt();
            if (spec.key == QStringLiteral("distance")) {
                if (index >= 0 && index < 4) {
                    criteria.maxDistanceKm = kDistanceOptionsKm[index]; // 单选：最多一个
                }
                continue;
            }
            const QString value = spec.options.value(index);
            if (spec.key == QStringLiteral("status")) {
                criteria.statuses << value;
            } else if (spec.key == QStringLiteral("operator")) {
                criteria.operators << value;
            } else if (spec.key == QStringLiteral("access")) {
                criteria.accessTypes << value;
            } else if (spec.key == QStringLiteral("parking")) {
                criteria.parkingFees << value;
            } else if (spec.key == QStringLiteral("feature")) {
                criteria.features << value;
            } else if (spec.key == QStringLiteral("chargerType")) {
                criteria.chargerTypes << value;
            } else if (spec.key == QStringLiteral("voltage")) {
                criteria.voltageBands << value;
            }
        }
    }
    return criteria;
}

} // namespace charging::client::pages::station
