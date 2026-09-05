#include "station_management_page.h"

#include "admin_request_gateway.h"
#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

#include <cmath>

namespace charging::server {

namespace {

constexpr int kPageSize = 10;

QString formatPriceCents(qint64 priceCents)
{
    return QString::number(priceCents / 100) + QStringLiteral(".")
        + QStringLiteral("%1").arg(priceCents % 100, 2, 10, QLatin1Char('0'));
}

bool parseCoordinate(const QString& text, double minimum, double maximum, double* coordinate)
{
    bool isValid = false;
    const double value = text.trimmed().toDouble(&isValid);
    if (!isValid || !std::isfinite(value) || value < minimum || value > maximum) {
        return false;
    }
    *coordinate = value;
    return true;
}

bool parsePriceCents(const QString& text, qint64* priceCents)
{
    static const QRegularExpression kPricePattern(
        QStringLiteral("^(0|[1-9][0-9]{0,3})(?:\\.([0-9]{1,2}))?$"));
    const QRegularExpressionMatch match = kPricePattern.match(text.trimmed());
    if (!match.hasMatch()) {
        return false;
    }

    bool isValid = false;
    const qint64 wholeUnits = match.captured(1).toLongLong(&isValid);
    if (!isValid) {
        return false;
    }
    const QString fraction = match.captured(2).leftJustified(2, QLatin1Char('0'));
    const qint64 fractionalCents = fraction.isEmpty() ? 0 : fraction.toLongLong(&isValid);
    if (!isValid) {
        return false;
    }
    *priceCents = wholeUnits * 100 + fractionalCents;
    return true;
}

QLabel* createTextLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

QString stationStatusStyle(const QString& status)
{
    if (status == QObject::tr("运营中")) {
        return QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("空闲")) {
        return QStringLiteral("background:#eaf2ff; color:#337df1; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    return QStringLiteral("background:#fff0f0; color:#ee5757; border-radius:6px; padding:0 7px;"
                          " font-size:12px; font-weight:600;");
}

QLabel* createStatusTag(const QString& status, QWidget* parent)
{
    auto* label = new QLabel(status, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(stationStatusStyle(status));
    return label;
}

QWidget* createCompactStatusTag(const QString& status, QWidget* parent)
{
    auto* label = createStatusTag(status, nullptr);
    label->setFixedSize(managementStatusTagWidth(status), 26);
    return createManagementTableCell(label, parent);
}

QFrame* createCompactCard(QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("contentCard"));
    return card;
}

} // namespace

StationManagementPage::StationManagementPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("stationManagementPage"));
    setMinimumWidth(kManagementPageMinimumWidth);
    setMinimumHeight(760);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(18);

    auto* metricsLayout = new QHBoxLayout();
    metricsLayout->setSpacing(16);
    metricsLayout->addWidget(createManagementMetricCard(
        tr("电站总数"), tr("328"), tr(" 座"), tr("较昨日  +12 (+3.80%)  ↑"), QColor("#347cf6"), 2, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("运营中电站"), tr("298"), tr(" 座"), tr("在线率  90.85%"), QColor("#43c7bc"), 3, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("电桩总数"), tr("2,186"), tr(" 台"), tr("较昨日  +48 (+2.24%)  ↑"), QColor("#ff9a26"), 2, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("今日订单数"), tr("1,256"), tr(" 单"), tr("较昨日  +168 (+15.46%)  ↑"), QColor("#43c7bc"), 1, this));
    layout->addLayout(metricsLayout);

    auto* toolbar = createCompactCard(this);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(18, 12, 18, 12);
    toolbarLayout->setSpacing(12);
    keywordLineEdit_ = new QLineEdit(toolbar);
    keywordLineEdit_->setMinimumWidth(180);
    keywordLineEdit_->setPlaceholderText(tr("⌕  搜索电站名称"));
    keywordLineEdit_->setAccessibleName(tr("电站名称或编号"));
    cityComboBox_ = new QComboBox(toolbar);
    cityComboBox_->addItems({tr("城市"), tr("杭州市"), tr("宁波市")});
    districtComboBox_ = new QComboBox(toolbar);
    districtComboBox_->addItems({tr("区域"), tr("余杭区"), tr("西湖区"), tr("滨江区"), tr("萧山区"), tr("拱墅区")});
    statusComboBox_ = new QComboBox(toolbar);
    statusComboBox_->addItems({tr("运营状态"), tr("运营中"), tr("空闲"), tr("已停用")});
    for (auto* comboBox : {cityComboBox_, districtComboBox_, statusComboBox_}) {
        comboBox->setMinimumWidth(132);
        configureManagementComboBox(comboBox);
    }
    auto* resetButton = new QPushButton(tr("重置"), toolbar);
    resetButton->setObjectName(QStringLiteral("secondaryButton"));
    auto* queryButton = new QPushButton(tr("查询"), toolbar);
    queryButton->setObjectName(QStringLiteral("primaryButton"));
    auto* addButton = new QPushButton(tr("新增电站"), toolbar);
    addButton->setObjectName(QStringLiteral("primaryButton"));
    addButton->setMinimumWidth(116);
    feedbackLabel_ = createTextLabel(tr("显示全部 328 座电站"),
                                     QStringLiteral("color:#6f7d92; font-size:13px;"), toolbar);
    feedbackLabel_->setFixedWidth(180);
    feedbackLabel_->setToolTip(feedbackLabel_->text());
    toolbarLayout->addWidget(keywordLineEdit_, 1);
    toolbarLayout->addWidget(cityComboBox_);
    toolbarLayout->addWidget(districtComboBox_);
    toolbarLayout->addWidget(statusComboBox_);
    toolbarLayout->addWidget(feedbackLabel_);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(queryButton);
    toolbarLayout->addWidget(addButton);
    layout->addWidget(toolbar);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(kManagementTableMinimumWidth);
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(18, 18, 18, 16);
    tableLayout->setSpacing(12);
    tableTitleLabel_ = createTextLabel(tr("电站列表（共 328 座）"),
                                       QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"), tableCard);
    tableLayout->addWidget(tableTitleLabel_);
    tableWidget_ = new QTableWidget(tableCard);
    tableWidget_->setColumnCount(10);
    tableWidget_->setHorizontalHeaderLabels(
        {tr("电站名称"), tr("城市 / 区域"), tr("详细地址"), tr("电桩数量"), tr("快充桩"),
         tr("慢充桩"), tr("今日订单"), tr("利用率"), tr("运营状态"), tr("操作")});
    tableWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget_->setFocusPolicy(Qt::NoFocus);
    tableWidget_->setAlternatingRowColors(true);
    tableWidget_->setShowGrid(false);
    tableWidget_->verticalHeader()->setVisible(false);
    tableWidget_->verticalHeader()->setDefaultSectionSize(49);
    tableWidget_->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    tableWidget_->horizontalHeader()->setStretchLastSection(false);
    tableWidget_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Fixed);
    tableWidget_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Fixed);
    // Four-character header “运营状态” also needs room for the header's own padding.
    tableWidget_->setColumnWidth(8, 84);
    tableWidget_->setColumnWidth(9, 132);
    tableLayout->addWidget(tableWidget_, 1);
    statePanel_ = new ManagementStatePanel(tableCard);
    tableLayout->addWidget(statePanel_);
    auto* pagerLayout = new QHBoxLayout();
    pagerLayout->addWidget(createTextLabel(tr("每页 10 条"), QStringLiteral("color:#718098; font-size:13px;"), tableCard));
    pagerLayout->addStretch();
    previousPageButton_ = new QPushButton(tr("‹"), tableCard);
    previousPageButton_->setObjectName(QStringLiteral("secondaryButton"));
    previousPageButton_->setFixedWidth(38);
    paginationLabel_ = createTextLabel(QString(), QStringLiteral("color:#34435b; font-size:13px; font-weight:600;"), tableCard);
    nextPageButton_ = new QPushButton(tr("›"), tableCard);
    nextPageButton_->setObjectName(QStringLiteral("secondaryButton"));
    nextPageButton_->setFixedWidth(38);
    pagerLayout->addWidget(previousPageButton_);
    pagerLayout->addWidget(paginationLabel_);
    pagerLayout->addWidget(nextPageButton_);
    tableLayout->addLayout(pagerLayout);
    contentLayout->addWidget(tableCard, 1);

    auto* detailCard = createManagementDetailCard(tr("电站详情"), this);
    detailCard->setFixedWidth(kManagementDetailWidth);
    auto* detailLayout = qobject_cast<QVBoxLayout*>(detailCard->layout());
    detailNameLabel_ = createTextLabel(QString(), QStringLiteral("color:#1d2c46; font-size:17px; font-weight:700;"), detailCard);
    detailLayout->addWidget(detailNameLabel_);
    auto* detailStateRow = new QHBoxLayout();
    detailStatusLabel_ = createStatusTag(tr("运营中"), detailCard);
    detailIdLabel_ = createTextLabel(QString(), QStringLiteral("color:#718098; font-size:13px;"), detailCard);
    detailStateRow->addWidget(detailStatusLabel_);
    detailStateRow->addSpacing(8);
    detailStateRow->addWidget(detailIdLabel_);
    detailStateRow->addStretch();
    detailLayout->addLayout(detailStateRow);
    detailLayout->addWidget(createTextLabel(tr("地址"), QStringLiteral("color:#6d7a90; font-size:13px; font-weight:600;"), detailCard));
    detailAddressLabel_ = createTextLabel(QString(), QStringLiteral("color:#55647c; font-size:13px;"), detailCard);
    detailAddressLabel_->setWordWrap(true);
    detailLayout->addWidget(detailAddressLabel_);
    detailContactLabel_ = createTextLabel(QString(), QStringLiteral("color:#55647c; font-size:13px;"), detailCard);
    detailContactLabel_->setWordWrap(true);
    detailLayout->addWidget(detailContactLabel_);
    auto* divider = new QFrame(detailCard);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color:#edf1f7;"));
    detailLayout->addWidget(divider);
    detailLayout->addWidget(createTextLabel(tr("电桩配置"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    detailConfigurationLabel_ = createTextLabel(QString(), QStringLiteral("color:#53627b; font-size:13px;"), detailCard);
    detailConfigurationLabel_->setAlignment(Qt::AlignCenter);
    detailLayout->addWidget(detailConfigurationLabel_);
    detailLayout->addWidget(createTextLabel(tr("实时状态"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    detailRealtimeLabel_ = createTextLabel(QString(), QStringLiteral("color:#53627b; font-size:13px;"), detailCard);
    detailRealtimeLabel_->setAlignment(Qt::AlignCenter);
    detailLayout->addWidget(detailRealtimeLabel_);
    detailLayout->addStretch();
    editButton_ = new QPushButton(tr("编辑电站"), detailCard);
    editButton_->setObjectName(QStringLiteral("primaryButton"));
    toggleStatusButton_ = new QPushButton(detailCard);
    toggleStatusButton_->setObjectName(QStringLiteral("secondaryButton"));
    detailLayout->addWidget(editButton_);
    detailLayout->addWidget(toggleStatusButton_);
    contentLayout->addWidget(detailCard);
    layout->addLayout(contentLayout, 1);

    connect(queryButton, &QPushButton::clicked, this, &StationManagementPage::applyFilters);
    connect(statePanel_, &ManagementStatePanel::resetRequested, this,
            &StationManagementPage::resetFilters);
    connect(statePanel_, &ManagementStatePanel::retryRequested, this,
            &StationManagementPage::applyFilters);
    connect(resetButton, &QPushButton::clicked, this, &StationManagementPage::resetFilters);
    connect(addButton, &QPushButton::clicked, this, &StationManagementPage::showAddStationDialog);
    connect(keywordLineEdit_, &QLineEdit::returnPressed, this, &StationManagementPage::applyFilters);
    connect(previousPageButton_, &QPushButton::clicked, this, &StationManagementPage::showPreviousPage);
    connect(nextPageButton_, &QPushButton::clicked, this, &StationManagementPage::showNextPage);
    connect(editButton_, &QPushButton::clicked, this, &StationManagementPage::showEditStationDialog);
    connect(toggleStatusButton_, &QPushButton::clicked, this, &StationManagementPage::toggleSelectedStationStatus);
    connect(tableWidget_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr) {
            showStationDetails(item->data(Qt::UserRole).toInt());
        }
    });
    createMockRecords();
    applyFilters();
}

void StationManagementPage::createMockRecords()
{
    records_ = {
        {tr("STN000328"), tr("未来科技城充电站"), tr("杭州市"), tr("余杭区"), tr("文一西路969号未来科技城通勤园停车场B区"), 30.2741, 120.0724, 128, tr("运营中"), 24, 18, 6, 68, 68, tr("张伟"), tr("138 6712 8888")},
        {tr("STN000329"), tr("西溪湿地公园充电站"), tr("杭州市"), tr("西湖区"), tr("天目山路518号西溪湿地北门"), 30.2675, 120.0819, 135, tr("运营中"), 16, 10, 6, 52, 62, tr("李娜"), tr("139 5800 2146")},
        {tr("STN000330"), tr("滨江星光大道充电站"), tr("杭州市"), tr("滨江区"), tr("江南大道228号星光大道地下车库"), 30.2086, 120.2103, 142, tr("运营中"), 20, 14, 6, 74, 71, tr("王凯"), tr("136 7574 6012")},
        {tr("STN000331"), tr("萧山机场P4充电站"), tr("杭州市"), tr("萧山区"), tr("机场高速萧山机场P4停车场"), 30.2296, 120.4341, 145, tr("运营中"), 30, 24, 6, 96, 75, tr("陈力"), tr("137 3815 3377")},
        {tr("STN000332"), tr("钱江世纪城充电站"), tr("杭州市"), tr("滨江区"), tr("民和路与奔竞大道交叉口"), 30.2324, 120.2189, 138, tr("运营中"), 18, 12, 6, 60, 65, tr("赵敏"), tr("135 8821 9104")},
        {tr("STN000333"), tr("城西银泰充电站"), tr("杭州市"), tr("拱墅区"), tr("丰潭路380号城西银泰城B2层"), 30.2897, 120.1116, 132, tr("空闲"), 12, 8, 4, 28, 53, tr("周辰"), tr("186 6804 2241")},
        {tr("STN000334"), tr("下沙大学城充电站"), tr("杭州市"), tr("余杭区"), tr("学源街998号下沙大学城东区"), 30.3217, 120.3529, 125, tr("运营中"), 22, 14, 8, 46, 49, tr("朱琳"), tr("158 5814 7732")},
        {tr("STN000335"), tr("临平地铁站充电站"), tr("杭州市"), tr("余杭区"), tr("临平地铁站南广场地下停车场"), 30.4218, 120.3006, 130, tr("空闲"), 16, 8, 8, 32, 48, tr("孙洋"), tr("150 6810 1835")},
        {tr("STN000336"), tr("宁波东部新城充电站"), tr("宁波市"), tr("鄞州区"), tr("宁穿路1888号东部新城停车楼"), 29.8637, 121.6198, 140, tr("已停用"), 14, 10, 4, 0, 0, tr("吴峰"), tr("188 5726 1300")},
        {tr("STN000337"), tr("宁波南部商务区充电站"), tr("宁波市"), tr("鄞州区"), tr("泰康中路558号南部商务区停车场"), 29.8264, 121.5502, 136, tr("运营中"), 20, 14, 6, 58, 63, tr("何静"), tr("189 5821 4650")},
    };
}

bool StationManagementPage::recordMatchesFilters(const StationRecord& record) const
{
    const QString keyword = keywordLineEdit_->text().trimmed();
    const bool matchesKeyword = keyword.isEmpty() || record.code.contains(keyword, Qt::CaseInsensitive)
        || record.name.contains(keyword, Qt::CaseInsensitive);
    const bool matchesCity = cityComboBox_->currentIndex() == 0 || record.city == cityComboBox_->currentText();
    const bool matchesDistrict = districtComboBox_->currentIndex() == 0 || record.district == districtComboBox_->currentText();
    const bool matchesStatus = statusComboBox_->currentIndex() == 0 || record.status == statusComboBox_->currentText();
    return matchesKeyword && matchesCity && matchesDistrict && matchesStatus;
}

void StationManagementPage::applyFilters()
{
    if (realMode_) { currentPage_ = 0; requestList(); return; }
    filteredRecordIndexes_.clear();
    for (int index = 0; index < records_.size(); ++index) {
        if (recordMatchesFilters(records_.at(index))) {
            filteredRecordIndexes_.append(index);
        }
    }
    currentPage_ = 0;
    rebuildTable();
    setFeedback(filteredRecordIndexes_.isEmpty() ? tr("未找到符合条件的电站")
                                                  : tr("筛选到 %1 座本地 Mock 电站").arg(filteredRecordIndexes_.size()));
}

void StationManagementPage::resetFilters()
{
    keywordLineEdit_->clear();
    cityComboBox_->setCurrentIndex(0);
    districtComboBox_->setCurrentIndex(0);
    statusComboBox_->setCurrentIndex(0);
    applyFilters();
    if (!realMode_) setFeedback(tr("已重置筛选条件，显示全部本地 Mock 电站"));
}

void StationManagementPage::rebuildTable()
{
    const int pageCount = realMode_ ? qMax(1, (totalRecords_ + kPageSize - 1) / kPageSize)
                                    : qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = realMode_ ? 0 : currentPage_ * kPageSize;
    const int end = realMode_ ? filteredRecordIndexes_.size() : qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const StationRecord& record = records_.at(recordIndex);
        const QList<QString> values = {record.name,
                                       realMode_ ? tr("契约未提供") : record.city + tr(" / ") + record.district,
                                       record.address, QString::number(record.chargerCount),
                                       realMode_ ? tr("—") : QString::number(record.fastChargerCount),
                                       realMode_ ? tr("—") : QString::number(record.slowChargerCount),
                                       realMode_ ? tr("—") : QString::number(record.todayOrders),
                                       realMode_ ? tr("—") : QString::number(record.utilizationPercent) + tr("%"), QString(), QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 8 || column == 9) {
                continue;
            }
            auto* item = createManagementTableItem(values.at(column));
            item->setData(Qt::UserRole, recordIndex);
            item->setTextAlignment(Qt::AlignCenter);
            tableWidget_->setItem(row, column, item);
        }
        tableWidget_->setCellWidget(row, 8, createCompactStatusTag(record.status, tableWidget_));
        auto* actions = new QWidget(tableWidget_);
        auto* actionLayout = new QHBoxLayout(actions);
        actionLayout->setContentsMargins(0, 0, 0, 0);
        actionLayout->setSpacing(4);
        auto* detailButton = new QPushButton(tr("详情"), actions);
        auto* editButton = new QPushButton(tr("编辑"), actions);
        for (auto* button : {detailButton, editButton}) {
            button->setObjectName(QStringLiteral("tableActionButton"));
            button->setCursor(Qt::PointingHandCursor);
            button->setFixedSize(40, 26);
            actionLayout->addWidget(button);
        }
        actionLayout->setAlignment(Qt::AlignCenter);
        connect(detailButton, &QPushButton::clicked, this, [this, recordIndex]() { showStationDetails(recordIndex); });
        connect(editButton, &QPushButton::clicked, this, [this, recordIndex]() {
            showStationDetails(recordIndex);
            showEditStationDialog();
        });
        tableWidget_->setCellWidget(row, 9, actions);
    }
    tableTitleLabel_->setText(tr("电站列表（共 %1 座）").arg(realMode_ ? totalRecords_ : filteredRecordIndexes_.size()));
    paginationLabel_->setText(tr("第 %1 / %2 页").arg(currentPage_ + 1).arg(pageCount));
    previousPageButton_->setEnabled(currentPage_ > 0);
    nextPageButton_->setEnabled(currentPage_ + 1 < pageCount);
    updateEmptyState();
    if (!filteredRecordIndexes_.isEmpty()) {
        if (!filteredRecordIndexes_.contains(selectedRecordIndex_)) {
            showStationDetails(filteredRecordIndexes_.first());
        } else {
            updateDetailActions();
        }
    } else {
        selectedRecordIndex_ = -1;
        detailNameLabel_->setText(tr("暂无匹配电站"));
        detailIdLabel_->clear();
        detailStatusLabel_->setText(tr("未选择"));
        detailStatusLabel_->setStyleSheet(QStringLiteral("background:#f1f4f8; color:#708096; border-radius:5px;"
                                                         " padding:4px 7px; font-size:13px; font-weight:600;"));
        detailAddressLabel_->setText(tr("请调整筛选条件后再查看详情。"));
        detailContactLabel_->clear();
        detailConfigurationLabel_->clear();
        detailRealtimeLabel_->clear();
        updateDetailActions();
    }
}

void StationManagementPage::updateEmptyState()
{
    const bool isEmpty = filteredRecordIndexes_.isEmpty();
    const bool hasFilter = !keywordLineEdit_->text().trimmed().isEmpty()
        || statusComboBox_->currentIndex() > 0;
    const auto state = !isEmpty ? ManagementListState::Hidden
        : realMode_ && !hasFilter ? ManagementListState::EmptyInitial
        : ManagementListState::EmptyFiltered;
    statePanel_->setState(state, realMode_ && !hasFilter
                                     ? tr("服务端当前没有电站记录。")
                                     : tr("当前筛选条件下没有电站。请调整条件或点击“重置”。"));
    tableWidget_->setVisible(!isEmpty);
    paginationLabel_->setVisible(!isEmpty);
    previousPageButton_->setVisible(!isEmpty);
    nextPageButton_->setVisible(!isEmpty);
}

void StationManagementPage::showStationDetails(int recordIndex)
{
    if (recordIndex < 0 || recordIndex >= records_.size()) {
        return;
    }
    selectedRecordIndex_ = recordIndex;
    const StationRecord& record = records_.at(recordIndex);
    if (realMode_ && gateway_) {
        detailRequestId_ = gateway_->request(QStringLiteral("stations.get"), {{QStringLiteral("id"), record.serverId}}, this,
                                             QStringLiteral("station-detail"));
    }
    detailNameLabel_->setText(record.name);
    detailIdLabel_->setText(tr("电站ID：%1").arg(record.code));
    detailStatusLabel_->setText(record.status);
    detailStatusLabel_->setStyleSheet(stationStatusStyle(record.status));
    detailAddressLabel_->setText(
        record.city + record.district + tr("\n") + record.address
        + tr("\n坐标　%1, %2\n电价　¥ %3 / kWh")
              .arg(QString::number(record.latitude, 'f', 6),
                   QString::number(record.longitude, 'f', 6), formatPriceCents(record.priceCentsPerKwh)));
    if (realMode_) {
        detailAddressLabel_->setText(tr("地址　%1\n坐标　%2, %3\n电价　¥ %4 / kWh\n记录更新时间　%5")
                                         .arg(record.address, QString::number(record.latitude, 'f', 6),
                                              QString::number(record.longitude, 'f', 6), formatPriceCents(record.priceCentsPerKwh),
                                              record.expectedUpdatedAt));
        detailContactLabel_->setText(tr("负责人及营业时间：契约未提供"));
        detailConfigurationLabel_->setText(tr("电桩总数　%1 台\n可用电桩数：由服务端列表摘要提供").arg(record.chargerCount));
        detailRealtimeLabel_->setText(tr("快慢充分类、今日订单与利用率：契约未提供"));
        updateDetailActions();
        return;
    }
    detailContactLabel_->setText(tr("营业时间　00:00 - 24:00\n负责人　　%1\n联系电话　%2")
                                     .arg(record.contactName, record.contactPhone));
    detailConfigurationLabel_->setText(tr("%1 台　　　 %2 台　　　 %3 台\n电桩总数　　 快充桩　　　慢充桩")
                                           .arg(record.chargerCount).arg(record.fastChargerCount).arg(record.slowChargerCount));
    const int charging = qMin(record.fastChargerCount, qMax(0, record.todayOrders / 6));
    const int free = qMax(0, record.chargerCount - charging - 4);
    detailRealtimeLabel_->setText(tr("%1 台　　　 %2 台　　　 4 台\n空闲　　　　 充电中　　　 故障")
                                      .arg(free).arg(charging));
    updateDetailActions();
    for (int row = 0; row < tableWidget_->rowCount(); ++row) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toInt() == recordIndex) {
            tableWidget_->selectRow(row);
            break;
        }
    }
}

void StationManagementPage::updateDetailActions()
{
    const bool hasSelection = selectedRecordIndex_ >= 0 && selectedRecordIndex_ < records_.size();
    editButton_->setEnabled(hasSelection);
    toggleStatusButton_->setEnabled(hasSelection);
    if (!hasSelection) {
        toggleStatusButton_->setText(tr("切换运营状态"));
        return;
    }
    const bool isStopped = records_.at(selectedRecordIndex_).status == tr("已停用");
    toggleStatusButton_->setText(isStopped ? tr("恢复运营") : tr("暂停运营"));
    toggleStatusButton_->setToolTip(realMode_ ? tr("受控状态操作；遇到活动订单会由服务端拒绝") : tr("仅更新本地 Mock 状态"));
}

void StationManagementPage::showAddStationDialog()
{
    if (realMode_) {
        QMessageBox::information(this, tr("当前不可用"), tr("本轮仅接入站点编辑和状态操作；新增站点暂不开放。"));
        return;
    }
    showStationDialog(-1);
}

void StationManagementPage::showEditStationDialog()
{
    if (selectedRecordIndex_ >= 0) {
        showStationDialog(selectedRecordIndex_);
    }
}

void StationManagementPage::showStationDialog(int recordIndex)
{
    const bool isEditing = recordIndex >= 0;
    QDialog dialog(this);
    dialog.setWindowTitle(isEditing ? (realMode_ ? tr("编辑电站") : tr("编辑电站（Mock）")) : tr("新增电站（Mock）"));
    dialog.setMinimumWidth(460);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#ffffff; color:#1d2c46; font-size:14px; }"
        "QLineEdit, QComboBox { background:#ffffff; border:1px solid #d0d7de; border-radius:9px;"
        " min-height:40px; padding:0 12px; font-size:14px; }"
        "QLineEdit:focus, QComboBox:focus { border:2px solid #2878d4; }"
        "QPushButton { background:#ffffff; border:1px solid #d0d7de; border-radius:8px; min-height:38px;"
        " padding:0 14px; font-size:15px; }"
        "QPushButton#primaryButton { background:#2878d4; border:none; color:#ffffff; min-height:42px; font-weight:600; }"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* formLayout = new QFormLayout();
    formLayout->setSpacing(12);
    auto* codeLineEdit = new QLineEdit(&dialog);
    auto* nameLineEdit = new QLineEdit(&dialog);
    auto* cityComboBox = new QComboBox(&dialog);
    cityComboBox->addItems({tr("杭州市"), tr("宁波市")});
    configureManagementComboBox(cityComboBox);
    auto* districtLineEdit = new QLineEdit(&dialog);
    auto* addressLineEdit = new QLineEdit(&dialog);
    auto* latitudeLineEdit = new QLineEdit(&dialog);
    auto* longitudeLineEdit = new QLineEdit(&dialog);
    auto* priceLineEdit = new QLineEdit(&dialog);
    auto* contactLineEdit = new QLineEdit(&dialog);
    auto* phoneLineEdit = new QLineEdit(&dialog);
    if (isEditing) {
        const StationRecord& record = records_.at(recordIndex);
        codeLineEdit->setText(record.code);
        codeLineEdit->setReadOnly(true);
        nameLineEdit->setText(record.name);
        cityComboBox->setCurrentText(record.city);
        districtLineEdit->setText(record.district);
        addressLineEdit->setText(record.address);
        latitudeLineEdit->setText(QString::number(record.latitude, 'f', 6));
        longitudeLineEdit->setText(QString::number(record.longitude, 'f', 6));
        priceLineEdit->setText(formatPriceCents(record.priceCentsPerKwh));
        contactLineEdit->setText(record.contactName);
        phoneLineEdit->setText(record.contactPhone);
        if (realMode_) {
            cityComboBox->setEnabled(false);
            districtLineEdit->setEnabled(false);
            contactLineEdit->setEnabled(false);
            phoneLineEdit->setEnabled(false);
            cityComboBox->setToolTip(tr("当前契约不支持此字段"));
            districtLineEdit->setToolTip(tr("当前契约不支持此字段"));
            contactLineEdit->setToolTip(tr("当前契约不支持此字段"));
            phoneLineEdit->setToolTip(tr("当前契约不支持此字段"));
        }
    } else {
        codeLineEdit->setPlaceholderText(tr("例如 STN000337"));
        nameLineEdit->setPlaceholderText(tr("例如 西湖文体中心充电站"));
        latitudeLineEdit->setPlaceholderText(tr("例如 30.274100"));
        longitudeLineEdit->setPlaceholderText(tr("例如 120.155100"));
        priceLineEdit->setPlaceholderText(tr("例如 1.28"));
    }
    formLayout->addRow(tr("电站编号 *"), codeLineEdit);
    formLayout->addRow(tr("电站名称 *"), nameLineEdit);
    formLayout->addRow(tr("城市 *"), cityComboBox);
    formLayout->addRow(tr("区域 *"), districtLineEdit);
    formLayout->addRow(tr("详细地址 *"), addressLineEdit);
    formLayout->addRow(tr("纬度 *（-90 ～ 90）"), latitudeLineEdit);
    formLayout->addRow(tr("经度 *（-180 ～ 180）"), longitudeLineEdit);
    formLayout->addRow(tr("电价 *（元 / kWh）"), priceLineEdit);
    formLayout->addRow(tr("负责人 *"), contactLineEdit);
    formLayout->addRow(tr("联系电话 *"), phoneLineEdit);
    layout->addLayout(formLayout);
    if (realMode_) {
        auto* hint = createTextLabel(tr("本次保存只提交名称、地址、经纬度和电价；城市、区域与联系人字段尚无管理契约，已禁用。"),
                                     QStringLiteral("color:#718098; font-size:13px;"), &dialog);
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(isEditing ? tr("保存修改") : tr("确认新增"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [&dialog, codeLineEdit, nameLineEdit, districtLineEdit, addressLineEdit,
             latitudeLineEdit, longitudeLineEdit, priceLineEdit, contactLineEdit, phoneLineEdit]() {
        if (codeLineEdit->text().trimmed().isEmpty() || nameLineEdit->text().trimmed().isEmpty()
            || districtLineEdit->text().trimmed().isEmpty() || addressLineEdit->text().trimmed().isEmpty()
            || contactLineEdit->text().trimmed().isEmpty() || phoneLineEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QObject::tr("请补全信息"), QObject::tr("所有带 * 的字段均为必填项。"));
            return;
        }
        double latitude = 0.0;
        double longitude = 0.0;
        qint64 priceCents = 0;
        if (!parseCoordinate(latitudeLineEdit->text(), -90.0, 90.0, &latitude)
            || !parseCoordinate(longitudeLineEdit->text(), -180.0, 180.0, &longitude)
            || !parsePriceCents(priceLineEdit->text(), &priceCents)) {
            QMessageBox::warning(&dialog, QObject::tr("输入格式不正确"),
                                 QObject::tr("请填写有效的经纬度，以及最多两位小数的非负电价。"));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    double latitude = 0.0;
    double longitude = 0.0;
    qint64 priceCents = 0;
    Q_ASSERT(parseCoordinate(latitudeLineEdit->text(), -90.0, 90.0, &latitude));
    Q_ASSERT(parseCoordinate(longitudeLineEdit->text(), -180.0, 180.0, &longitude));
    Q_ASSERT(parsePriceCents(priceLineEdit->text(), &priceCents));
    if (realMode_) {
        const auto& record = records_.at(recordIndex);
        writeRequestId_ = gateway_->request(QStringLiteral("station.edit"),
            {{QStringLiteral("operationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
             {QStringLiteral("id"), record.serverId},
             {QStringLiteral("expectedUpdatedAt"), record.expectedUpdatedAt},
             {QStringLiteral("name"), nameLineEdit->text().trimmed()},
             {QStringLiteral("address"), addressLineEdit->text().trimmed()},
             {QStringLiteral("latitude"), latitude}, {QStringLiteral("longitude"), longitude},
             {QStringLiteral("priceCentsPerKwh"), priceCents}}, this, QStringLiteral("station-write"));
        setFeedback(tr("正在提交电站编辑…"));
        return;
    }
    if (isEditing) {
        StationRecord& record = records_[recordIndex];
        record.name = nameLineEdit->text().trimmed();
        record.city = cityComboBox->currentText();
        record.district = districtLineEdit->text().trimmed();
        record.address = addressLineEdit->text().trimmed();
        record.latitude = latitude;
        record.longitude = longitude;
        record.priceCentsPerKwh = priceCents;
        record.contactName = contactLineEdit->text().trimmed();
        record.contactPhone = phoneLineEdit->text().trimmed();
        selectedRecordIndex_ = recordIndex;
        applyFilters();
        showStationDetails(recordIndex);
        setFeedback(tr("已保存 %1 的本地 Mock 修改").arg(record.code));
        return;
    }
    const QString code = codeLineEdit->text().trimmed();
    for (const StationRecord& record : records_) {
        if (record.code.compare(code, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, tr("编号重复"), tr("该电站编号已存在，请使用新的编号。"));
            return;
        }
    }
    records_.append({code, nameLineEdit->text().trimmed(), cityComboBox->currentText(),
                     districtLineEdit->text().trimmed(), addressLineEdit->text().trimmed(), latitude,
                     longitude, priceCents, tr("运营中"), 12, 8, 4, 0, 0,
                     contactLineEdit->text().trimmed(), phoneLineEdit->text().trimmed()});
    selectedRecordIndex_ = records_.size() - 1;
    applyFilters();
    showStationDetails(selectedRecordIndex_);
    setFeedback(tr("已新增 %1（仅本地 Mock）").arg(code));
}

void StationManagementPage::toggleSelectedStationStatus()
{
    if (selectedRecordIndex_ < 0 || selectedRecordIndex_ >= records_.size()) {
        return;
    }
    StationRecord& record = records_[selectedRecordIndex_];
    const bool isStopped = record.status == tr("已停用");
    const auto choice = QMessageBox::question(
        this, isStopped ? tr("确认恢复运营") : tr("确认暂停运营"),
        realMode_ ? (isStopped ? tr("确认恢复电站 %1 的运营吗？服务端会校验当前版本与业务占用。").arg(record.name)
                               : tr("确认暂停电站 %1 的运营吗？服务端会校验活动预约、充电和当前版本。").arg(record.name))
                  : (isStopped ? tr("确认恢复电站 %1 的运营吗？该操作仅更新本地 Mock 状态。\n真实操作需等待 Service 接口。").arg(record.name)
                               : tr("确认暂停电站 %1 的运营吗？该操作仅更新本地 Mock 状态。\n真实操作需等待 Service 接口。").arg(record.name)));
    if (choice != QMessageBox::Yes) {
        return;
    }
    if (realMode_) {
        writeRequestId_ = gateway_->request(QStringLiteral("station.status"),
            {{QStringLiteral("operationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
             {QStringLiteral("id"), record.serverId}, {QStringLiteral("expectedUpdatedAt"), record.expectedUpdatedAt},
             {QStringLiteral("status"), isStopped ? QStringLiteral("ACTIVE") : QStringLiteral("INACTIVE")}}, this,
            QStringLiteral("station-write"));
        setFeedback(tr("正在提交电站状态更新…"));
        return;
    }
    record.status = isStopped ? tr("运营中") : tr("已停用");
    applyFilters();
    showStationDetails(selectedRecordIndex_);
    setFeedback(tr("已%1 %2（仅本地 Mock）").arg(isStopped ? tr("恢复运营") : tr("暂停运营"), record.name));
}

void StationManagementPage::showPreviousPage()
{
    if (currentPage_ <= 0) {
        return;
    }
    --currentPage_;
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void StationManagementPage::showNextPage()
{
    const int pageCount = realMode_ ? (totalRecords_ + kPageSize - 1) / kPageSize
                                    : (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize;
    if (currentPage_ + 1 >= pageCount) {
        return;
    }
    ++currentPage_;
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void StationManagementPage::setFeedback(const QString& text)
{
    feedbackLabel_->setText(text);
    feedbackLabel_->setToolTip(text);
}

void StationManagementPage::setAdminGateway(AdminRequestGateway* gateway)
{
    gateway_ = gateway; realMode_ = gateway_ != nullptr;
    if (!gateway_) return;
    cityComboBox_->setEnabled(false); cityComboBox_->setToolTip(tr("当前契约不支持按城市筛选"));
    districtComboBox_->setEnabled(false); districtComboBox_->setToolTip(tr("当前契约不支持按区域筛选"));
    statusComboBox_->removeItem(2); // "空闲" is not a station-status contract value.
    for (auto* button : findChildren<QPushButton*>()) {
        if (button->text() == tr("新增电站")) {
            button->setEnabled(false);
            button->setToolTip(tr("本轮仅接入站点编辑和状态操作；新增站点暂不开放。"));
        }
    }
    setManagementMetricCardsUnavailable(this, tr("当前契约未提供电站页汇总指标"));
    connect(gateway_, &AdminRequestGateway::finished, this, [this](const QString& id, const QJsonObject& response) {
        if (id == listRequestId_) handleListResponse(response);
        else if (id == writeRequestId_) handleWriteResponse(response);
        else if (id == detailRequestId_ && !response.value(QStringLiteral("success")).toBool())
            setFeedback(tr("详情确认失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()));
    });
    connect(gateway_, &AdminRequestGateway::authenticationChanged, this, [this](bool authenticated) {
        if (authenticated) requestList();
    });
    requestList();
}

QString StationManagementPage::statusCode(const QString& display) const
{
    if (display == tr("运营中") || display == tr("空闲")) return QStringLiteral("ACTIVE");
    if (display == tr("已停用")) return QStringLiteral("INACTIVE");
    return {};
}

void StationManagementPage::requestList()
{
    if (!gateway_ || !gateway_->isAuthenticated()) return;
    QJsonObject query{{QStringLiteral("page"), currentPage_ + 1}, {QStringLiteral("pageSize"), kPageSize}, {QStringLiteral("sort"), QStringLiteral("idDesc")}};
    const auto keyword = keywordLineEdit_->text().trimmed(); if (!keyword.isEmpty()) query.insert(QStringLiteral("keyword"), keyword);
    if (const auto status = statusCode(statusComboBox_->currentText()); !status.isEmpty()) query.insert(QStringLiteral("status"), status);
    listRequestId_ = gateway_->request(QStringLiteral("stations.list"), query, this, QStringLiteral("station-list"));
    setFeedback(tr("正在加载服务数据…"));
}

void StationManagementPage::handleListResponse(const QJsonObject& response)
{
    records_.clear(); filteredRecordIndexes_.clear(); selectedRecordIndex_ = -1;
    if (!response.value(QStringLiteral("success")).toBool()) {
        totalRecords_ = 0; rebuildTable();
        setFeedback(tr("加载失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString())); return;
    }
    const auto data = response.value(QStringLiteral("data")).toObject(); totalRecords_ = data.value(QStringLiteral("total")).toInt();
    for (const auto& value : data.value(QStringLiteral("items")).toArray()) {
        const auto item = value.toObject(); const bool active = item.value(QStringLiteral("status")).toString() == QStringLiteral("ACTIVE");
        records_.append({item.value(QStringLiteral("code")).toString(), item.value(QStringLiteral("name")).toString(), tr("—"), tr("—"),
            item.value(QStringLiteral("address")).toString(), item.value(QStringLiteral("latitude")).toDouble(), item.value(QStringLiteral("longitude")).toDouble(),
            item.value(QStringLiteral("priceCentsPerKwh")).toInteger(), active ? tr("运营中") : tr("已停用"), item.value(QStringLiteral("totalChargers")).toInt(), 0, 0, 0, 0,
            tr("契约未提供"), tr("—"), item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("updatedAt")).toString()});
        filteredRecordIndexes_.append(records_.size() - 1);
    }
    rebuildTable();
    setManagementMetricCardValue(this, 0, tr("%1 座").arg(totalRecords_),
                                 tr("服务端分页总数（当前筛选）"));
    setFeedback(totalRecords_ ? tr("已加载 %1 座电站（服务端分页）").arg(totalRecords_) : tr("当前没有电站数据"));
}

void StationManagementPage::handleWriteResponse(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) { setFeedback(tr("操作未完成：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString())); return; }
    setFeedback(tr("操作已提交，正在刷新服务数据…")); requestList();
}

} // namespace charging::server
