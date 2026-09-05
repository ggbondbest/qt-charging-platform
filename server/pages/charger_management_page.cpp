#include "charger_management_page.h"

#include "admin_request_gateway.h"
#include "dashboard_visual_widgets.h"
#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QList>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

namespace charging::server {

namespace {

constexpr int kPageSize = 10;

QLabel* createTextLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

QString statusStyle(const QString& status)
{
    if (status == QObject::tr("充电中")) {
        return QStringLiteral("background:#fff3df; color:#f08a1c; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("可用")) {
        return QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("已预约")) {
        return QStringLiteral("background:#eaf2ff; color:#337df1; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("故障")) {
        return QStringLiteral("background:#fff0f0; color:#ee5757; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    return QStringLiteral("background:#f1f4f8; color:#708096; border-radius:6px; padding:0 7px;"
                          " font-size:12px; font-weight:600;");
}

QLabel* createStatusTag(const QString& status, QWidget* parent)
{
    auto* label = new QLabel(status, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(statusStyle(status));
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

ChargerManagementPage::ChargerManagementPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("chargerManagementPage"));
    setMinimumWidth(kManagementPageMinimumWidth);
    setMinimumHeight(770);
    createMockRecords();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(18);

    auto* metricsLayout = new QHBoxLayout();
    metricsLayout->setSpacing(16);
    metricsLayout->addWidget(createManagementMetricCard(
        tr("电桩总数"), tr("1,522"), tr(" 台"), tr("较昨日  +18 (+1.20%)  ↑"),
        QColor("#347cf6"), 2, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("在线电桩"), tr("1,256"), tr(" 台"), tr("在线率  82.46%"), QColor("#43c7bc"), 3, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("故障电桩"), tr("88"), tr(" 台"), tr("故障率  5.78%"), QColor("#ff9a26"), 3, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("今日充电次数"), tr("3,842"), tr(" 次"), tr("较昨日  +256 (+7.14%)  ↑"),
        QColor("#43c7bc"), 1, this));
    layout->addLayout(metricsLayout);

    auto* toolbar = createCompactCard(this);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(18, 12, 18, 12);
    toolbarLayout->setSpacing(10);

    keywordLineEdit_ = new QLineEdit(toolbar);
    keywordLineEdit_->setMinimumWidth(188);
    keywordLineEdit_->setPlaceholderText(tr("⌕  搜索电桩编号"));
    keywordLineEdit_->setAccessibleName(tr("电桩编号关键字"));
    stationComboBox_ = new QComboBox(toolbar);
    stationComboBox_->addItems({tr("所属电站"), tr("未来科技城充电站"),
                                tr("滨江智慧园充电站"), tr("城西银泰充电站"),
                                tr("奥体中心充电站"), tr("萧山机场充电站"),
                                tr("富阳智造港充电站")});
    statusComboBox_ = new QComboBox(toolbar);
    statusComboBox_->addItems({tr("状态"), tr("异常电桩"), tr("可用"), tr("已预约"),
                               tr("充电中"), tr("故障"), tr("离线")});
    typeComboBox_ = new QComboBox(toolbar);
    typeComboBox_->addItems({tr("类型"), tr("直流桩"), tr("交流桩")});
    powerComboBox_ = new QComboBox(toolbar);
    powerComboBox_->addItems({tr("功率"), tr("7kW"), tr("60kW"), tr("120kW"), tr("180kW")});
    for (auto* comboBox : {stationComboBox_, statusComboBox_, typeComboBox_, powerComboBox_}) {
        comboBox->setMinimumWidth(112);
        configureManagementComboBox(comboBox);
    }
    auto* resetButton = new QPushButton(tr("重置"), toolbar);
    resetButton->setObjectName(QStringLiteral("secondaryButton"));
    auto* queryButton = new QPushButton(tr("查询"), toolbar);
    queryButton->setObjectName(QStringLiteral("primaryButton"));
    queryButton->setMinimumWidth(74);
    auto* addButton = new QPushButton(tr("新增电桩"), toolbar);
    addButton->setObjectName(QStringLiteral("primaryButton"));
    feedbackLabel_ = createTextLabel(tr("显示全部 1,522 台电桩"),
                                     QStringLiteral("color:#6f7d92; font-size:13px;"), toolbar);
    feedbackLabel_->setFixedWidth(180);
    feedbackLabel_->setToolTip(feedbackLabel_->text());

    toolbarLayout->addWidget(keywordLineEdit_, 1);
    toolbarLayout->addWidget(stationComboBox_);
    toolbarLayout->addWidget(statusComboBox_);
    toolbarLayout->addWidget(typeComboBox_);
    toolbarLayout->addWidget(powerComboBox_);
    toolbarLayout->addWidget(feedbackLabel_);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(queryButton);
    toolbarLayout->addWidget(addButton);
    layout->addWidget(toolbar);

    auto* bodyLayout = new QHBoxLayout();
    bodyLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(kManagementTableMinimumWidth);
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(18, 18, 18, 16);
    tableLayout->setSpacing(12);
    tableTitleLabel_ = createTextLabel(tr("电桩列表（共 1,522 台）"),
                                       QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"),
                                       tableCard);
    tableLayout->addWidget(tableTitleLabel_);

    tableWidget_ = new QTableWidget(tableCard);
    tableWidget_->setColumnCount(11);
    tableWidget_->setHorizontalHeaderLabels(
        {tr("电桩编号"), tr("所属电站"), tr("类型"), tr("功率"), tr("状态"),
         tr("异常分类（状态）"), tr("今日次数（未提供）"), tr("累计次数"), tr("累计时长"), tr("记录更新时间"), tr("操作")});
    tableWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget_->setFocusPolicy(Qt::NoFocus);
    tableWidget_->setAlternatingRowColors(true);
    tableWidget_->setShowGrid(false);
    tableWidget_->verticalHeader()->setVisible(false);
    tableWidget_->verticalHeader()->setDefaultSectionSize(46);
    tableWidget_->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    tableWidget_->horizontalHeader()->setStretchLastSection(false);
    tableWidget_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    tableWidget_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(10, QHeaderView::Fixed);
    tableWidget_->setColumnWidth(4, kManagementStatusColumnWidth);
    tableWidget_->setColumnWidth(10, 84);
    tableLayout->addWidget(tableWidget_, 1);

    statePanel_ = new ManagementStatePanel(tableCard);
    tableLayout->addWidget(statePanel_);

    auto* pagerLayout = new QHBoxLayout();
    pagerLayout->addWidget(createTextLabel(tr("每页 10 条"),
                                           QStringLiteral("color:#718098; font-size:13px;"), tableCard));
    pagerLayout->addStretch();
    previousPageButton_ = new QPushButton(tr("‹"), tableCard);
    previousPageButton_->setObjectName(QStringLiteral("secondaryButton"));
    previousPageButton_->setFixedWidth(38);
    previousPageButton_->setAccessibleName(tr("上一页"));
    paginationLabel_ = createTextLabel(QString(),
                                       QStringLiteral("color:#34435b; font-size:13px; font-weight:600;"),
                                       tableCard);
    nextPageButton_ = new QPushButton(tr("›"), tableCard);
    nextPageButton_->setObjectName(QStringLiteral("secondaryButton"));
    nextPageButton_->setFixedWidth(38);
    nextPageButton_->setAccessibleName(tr("下一页"));
    pagerLayout->addWidget(previousPageButton_);
    pagerLayout->addWidget(paginationLabel_);
    pagerLayout->addWidget(nextPageButton_);
    tableLayout->addLayout(pagerLayout);
    bodyLayout->addWidget(tableCard, 1);

    auto* detailCard = createManagementDetailCard(tr("电桩详情"), this);
    detailCard->setFixedWidth(kManagementDetailWidth);
    auto* detailLayout = qobject_cast<QVBoxLayout*>(detailCard->layout());
    auto* detailTitleRow = new QHBoxLayout();
    detailCodeLabel_ = createTextLabel(QString(),
                                       QStringLiteral("color:#273751; font-size:15px; font-weight:700;"), detailCard);
    detailStatusLabel_ = createStatusTag(tr("可用"), detailCard);
    detailTitleRow->addWidget(detailCodeLabel_);
    detailTitleRow->addStretch();
    detailTitleRow->addWidget(detailStatusLabel_);
    detailLayout->addLayout(detailTitleRow);
    detailStationLabel_ = createTextLabel(QString(),
                                          QStringLiteral("color:#718098; font-size:13px;"), detailCard);
    detailStationLabel_->setWordWrap(true);
    detailLayout->addWidget(detailStationLabel_);
    auto* divider = new QFrame(detailCard);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color:#edf1f7;"));
    detailLayout->addWidget(divider);
    detailLayout->addWidget(createTextLabel(tr("基本信息"),
                                            QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"),
                                            detailCard));
    detailBasicInfoLabel_ = createTextLabel(QString(),
                                            QStringLiteral("color:#65738a; font-size:13px;"), detailCard);
    detailBasicInfoLabel_->setWordWrap(true);
    detailLayout->addWidget(detailBasicInfoLabel_);
    detailLayout->addWidget(createTextLabel(tr("运行状态"),
                                            QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"),
                                            detailCard));
    detailRuntimeInfoLabel_ = createTextLabel(QString(),
                                              QStringLiteral("color:#65738a; font-size:13px;"), detailCard);
    detailRuntimeInfoLabel_->setWordWrap(true);
    detailLayout->addWidget(detailRuntimeInfoLabel_);
    detailLayout->addStretch();
    restartButton_ = new QPushButton(tr("远程重启"), detailCard);
    restartButton_->setObjectName(QStringLiteral("primaryButton"));
    restartButton_->setAccessibleName(tr("远程重启当前电桩"));
    refreshStatusButton_ = new QPushButton(tr("刷新状态"), detailCard);
    refreshStatusButton_->setObjectName(QStringLiteral("secondaryButton"));
    clearAlertButton_ = new QPushButton(tr("解除告警"), detailCard);
    clearAlertButton_->setStyleSheet(QStringLiteral("QPushButton { background:white; color:#f05d62;"
                                                     " border:1px solid #ffc9cb; border-radius:8px; min-height:38px;"
                                                     " font-size:15px; } QPushButton:hover { background:#fff5f5; }"));
    clearAlertButton_->setAccessibleName(tr("解除当前电桩告警"));
    editButton_ = new QPushButton(tr("编辑电桩"), detailCard);
    editButton_->setObjectName(QStringLiteral("secondaryButton"));
    editButton_->setAccessibleName(tr("编辑当前电桩的本地 Mock 信息"));
    detailLayout->addWidget(restartButton_);
    detailLayout->addWidget(refreshStatusButton_);
    detailLayout->addWidget(clearAlertButton_);
    detailLayout->addWidget(editButton_);
    bodyLayout->addWidget(detailCard);
    layout->addLayout(bodyLayout, 1);

    connect(queryButton, &QPushButton::clicked, this, &ChargerManagementPage::applyFilters);
    connect(statePanel_, &ManagementStatePanel::resetRequested, this,
            &ChargerManagementPage::resetFilters);
    connect(statePanel_, &ManagementStatePanel::retryRequested, this,
            &ChargerManagementPage::applyFilters);
    connect(resetButton, &QPushButton::clicked, this, &ChargerManagementPage::resetFilters);
    connect(keywordLineEdit_, &QLineEdit::returnPressed, this, &ChargerManagementPage::applyFilters);
    connect(previousPageButton_, &QPushButton::clicked, this, &ChargerManagementPage::showPreviousPage);
    connect(nextPageButton_, &QPushButton::clicked, this, &ChargerManagementPage::showNextPage);
    connect(restartButton_, &QPushButton::clicked, this, &ChargerManagementPage::restartSelectedCharger);
    connect(refreshStatusButton_, &QPushButton::clicked, this,
            &ChargerManagementPage::refreshSelectedStatus);
    connect(clearAlertButton_, &QPushButton::clicked, this,
            &ChargerManagementPage::clearSelectedAlert);
    connect(addButton, &QPushButton::clicked, this, &ChargerManagementPage::showAddChargerDialog);
    connect(editButton_, &QPushButton::clicked, this, &ChargerManagementPage::showEditChargerDialog);
    connect(tableWidget_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr) {
            showChargerDetails(item->data(Qt::UserRole).toInt());
        }
    });

    applyFilters();
}

void ChargerManagementPage::createMockRecords()
{
    records_ = admin_mock::createChargerRecords();
}

void ChargerManagementPage::showExceptionRecords()
{
    keywordLineEdit_->clear();
    stationComboBox_->setCurrentIndex(0);
    statusComboBox_->setCurrentText(tr("异常电桩"));
    typeComboBox_->setCurrentIndex(0);
    powerComboBox_->setCurrentIndex(0);
    applyFilters();
    if (!realMode_)
        setFeedback(tr("正在显示 %1 台有活动异常的本地 Mock 电桩").arg(filteredRecordIndexes_.size()));
}

void ChargerManagementPage::showExceptionRecord(const QString& chargerCode)
{
    showExceptionRecords();
    keywordLineEdit_->setText(chargerCode);
    applyFilters();
    if (!filteredRecordIndexes_.isEmpty()) {
        showChargerDetails(filteredRecordIndexes_.first());
        setFeedback(tr("已定位到 %1 的异常详情").arg(chargerCode));
    }
}

bool ChargerManagementPage::recordMatchesFilters(const ChargerRecord& record) const
{
    const QString keyword = keywordLineEdit_->text().trimmed();
    const bool matchesKeyword = keyword.isEmpty() || record.code.contains(keyword, Qt::CaseInsensitive);
    const bool matchesStation = stationComboBox_->currentIndex() == 0
        || record.station == stationComboBox_->currentText();
    const bool requestsExceptions = statusComboBox_->currentText() == tr("异常电桩");
    const bool matchesStatus = statusComboBox_->currentIndex() == 0
        || (requestsExceptions ? !record.alertType.isEmpty()
                               : record.status == statusComboBox_->currentText());
    const bool matchesType = typeComboBox_->currentIndex() == 0
        || record.type == typeComboBox_->currentText();
    const bool matchesPower = powerComboBox_->currentIndex() == 0
        || record.power == powerComboBox_->currentText();
    return matchesKeyword && matchesStation && matchesStatus && matchesType && matchesPower;
}

void ChargerManagementPage::applyFilters()
{
    if (realMode_) {
        currentPage_ = 0;
        requestList();
        return;
    }
    filteredRecordIndexes_.clear();
    for (int index = 0; index < records_.size(); ++index) {
        if (recordMatchesFilters(records_.at(index))) {
            filteredRecordIndexes_.append(index);
        }
    }
    currentPage_ = 0;
    rebuildTable();
    setFeedback(filteredRecordIndexes_.isEmpty()
                    ? tr("未找到符合条件的电桩")
                    : tr("筛选到 %1 台本地 Mock 电桩").arg(filteredRecordIndexes_.size()));
}

void ChargerManagementPage::resetFilters()
{
    keywordLineEdit_->clear();
    stationComboBox_->setCurrentIndex(0);
    statusComboBox_->setCurrentIndex(0);
    typeComboBox_->setCurrentIndex(0);
    powerComboBox_->setCurrentIndex(0);
    applyFilters();
    if (!realMode_)
        setFeedback(tr("已重置筛选条件，显示全部本地 Mock 电桩"));
}

void ChargerManagementPage::rebuildTable()
{
    const int pageCount = realMode_ ? qMax(1, (totalRecords_ + kPageSize - 1) / kPageSize)
                                    : qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = realMode_ ? 0 : currentPage_ * kPageSize;
    const int end = realMode_ ? filteredRecordIndexes_.size()
                              : qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const ChargerRecord& record = records_.at(recordIndex);
        const QList<QString> values = {record.code, record.station, record.type, record.power, QString(),
                                       record.alertType.isEmpty() ? tr("—") : record.alertType,
                                       realMode_ ? tr("—") : QString::number(record.todaySessions),
                                       QString::number(record.totalSessions), record.totalDuration,
                                       record.lastHeartbeat, QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 4 || column == 10) {
                continue;
            }
            auto* item = createManagementTableItem(values.at(column));
            item->setData(Qt::UserRole, recordIndex);
            item->setTextAlignment(Qt::AlignCenter);
            if (column == 5 && !record.alertType.isEmpty()) {
                item->setForeground(record.status == tr("离线") ? QColor("#748196")
                                                                 : QColor("#ee5757"));
            }
            tableWidget_->setItem(row, column, item);
        }
        tableWidget_->setCellWidget(row, 4, createCompactStatusTag(record.status, tableWidget_));
        auto* detailButton = new QPushButton(tr("详情"), tableWidget_);
        detailButton->setObjectName(QStringLiteral("tableActionButton"));
        detailButton->setAccessibleName(tr("查看 %1 的详情").arg(record.code));
        connect(detailButton, &QPushButton::clicked, this, [this, recordIndex]() {
            showChargerDetails(recordIndex);
        });
        tableWidget_->setCellWidget(row, 10, createManagementTableCell(detailButton, tableWidget_));
    }
    tableTitleLabel_->setText(tr("电桩列表（共 %1 台）").arg(realMode_ ? totalRecords_ : filteredRecordIndexes_.size()));
    paginationLabel_->setText(tr("第 %1 / %2 页").arg(currentPage_ + 1).arg(pageCount));
    previousPageButton_->setEnabled(currentPage_ > 0);
    nextPageButton_->setEnabled(currentPage_ + 1 < pageCount);
    updateEmptyState();
    if (!filteredRecordIndexes_.isEmpty()) {
        if (!filteredRecordIndexes_.contains(selectedRecordIndex_)) {
            showChargerDetails(filteredRecordIndexes_.first());
        } else {
            updateDetailActions();
        }
    } else {
        selectedRecordIndex_ = -1;
        detailCodeLabel_->setText(tr("暂无匹配电桩"));
        detailStationLabel_->setText(tr("请调整筛选条件后再查看详情。"));
        detailStatusLabel_->setText(tr("未选择"));
        detailStatusLabel_->setStyleSheet(QStringLiteral("background:#f1f4f8; color:#708096; border-radius:5px;"
                                                         " padding:4px 7px; font-size:13px; font-weight:600;"));
        detailBasicInfoLabel_->clear();
        detailRuntimeInfoLabel_->clear();
        updateDetailActions();
    }
}

void ChargerManagementPage::updateEmptyState()
{
    const bool isEmpty = filteredRecordIndexes_.isEmpty();
    const bool hasFilter = !keywordLineEdit_->text().trimmed().isEmpty()
        || statusComboBox_->currentIndex() > 0 || typeComboBox_->currentIndex() > 0;
    const auto state = !isEmpty ? ManagementListState::Hidden
        : realMode_ && !hasFilter ? ManagementListState::EmptyInitial
        : ManagementListState::EmptyFiltered;
    statePanel_->setState(state, realMode_ && !hasFilter
                                     ? tr("服务端当前没有电桩记录。")
                                     : tr("当前筛选条件下没有电桩。请调整条件或点击“重置”。"));
    tableWidget_->setVisible(!isEmpty);
    paginationLabel_->setVisible(!isEmpty);
    previousPageButton_->setVisible(!isEmpty);
    nextPageButton_->setVisible(!isEmpty);
}

void ChargerManagementPage::showChargerDetails(int recordIndex)
{
    if (recordIndex < 0 || recordIndex >= records_.size()) {
        return;
    }
    selectedRecordIndex_ = recordIndex;
    const ChargerRecord& record = records_.at(recordIndex);
    if (realMode_ && gateway_) {
        detailRequestId_ = gateway_->request(QStringLiteral("chargers.get"), {{QStringLiteral("id"), record.serverId}}, this,
                                             QStringLiteral("charger-detail"));
    }
    detailCodeLabel_->setText(record.code);
    detailStatusLabel_->setText(record.status);
    detailStatusLabel_->setStyleSheet(statusStyle(record.status));
    detailStationLabel_->setText(record.station + tr("\n更新时间：") + record.lastHeartbeat);
    if (realMode_) {
        detailBasicInfoLabel_->setText(tr("电桩类型　%1\n额定功率　%2\n服务端 ID　%3")
                                           .arg(record.type, record.power, record.serverId));
        detailRuntimeInfoLabel_->setText(tr("累计次数　%1 次\n累计时长　%2\n当前状态　%3\n异常分类　%4\n记录更新时间　%5")
                                             .arg(QString::number(record.totalSessions), record.totalDuration, record.status,
                                                  record.alertType.isEmpty() ? tr("无（契约不记录诊断事件）") : record.alertType,
                                                  record.lastHeartbeat));
        updateDetailActions();
        return;
    }
    detailBasicInfoLabel_->setText(
        tr("电桩类型　%1\n额定功率　%2\n额定电压　750V\n设备版本　V2.3.6\n出厂编号　202408060086")
            .arg(record.type, record.power));
    const QString alertSummary = record.alertType.isEmpty()
        ? tr("无")
        : tr("%1（%2）").arg(record.alertType, record.alertOccurredAt);
    detailRuntimeInfoLabel_->setText(
        tr("今日次数　%1 次\n累计次数　%2 次\n累计时长　%3\n当前功率　%4\n最近异常　%5\n最后心跳　%6")
            .arg(QString::number(record.todaySessions), QString::number(record.totalSessions),
                 record.totalDuration, record.status == tr("充电中") ? tr("68.4 kW") : tr("--"),
                 alertSummary, record.lastHeartbeat));
    updateDetailActions();
    for (int row = 0; row < tableWidget_->rowCount(); ++row) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toInt() == recordIndex) {
            tableWidget_->selectRow(row);
            break;
        }
    }
}

void ChargerManagementPage::updateDetailActions()
{
    const bool hasSelection = selectedRecordIndex_ >= 0 && selectedRecordIndex_ < records_.size();
    const QString status = hasSelection ? records_.at(selectedRecordIndex_).status : QString();
    const bool canRestart = hasSelection && status != tr("充电中") && status != tr("已预约");
    restartButton_->setEnabled(canRestart);
    restartButton_->setToolTip(canRestart ? (realMode_ ? tr("受控状态模拟；不会向真实硬件发送命令") : tr("仅更新本地 Mock 状态"))
                                          : tr("充电中或已预约的电桩不可远程重启"));
    refreshStatusButton_->setEnabled(hasSelection);
    const bool canClearAlert = hasSelection && (status == tr("故障") || status == tr("离线"));
    clearAlertButton_->setEnabled(!realMode_ && canClearAlert);
    clearAlertButton_->setToolTip(realMode_ ? tr("当前契约不支持“解除告警”；可使用受控重启恢复为可用") : (canClearAlert ? tr("仅解除本地 Mock 告警")
                                                : tr("当前电桩没有可解除的告警")));
    if (realMode_) {
        editButton_->setText(tr("设置设备状态"));
        editButton_->setEnabled(hasSelection && status != tr("充电中") && status != tr("已预约"));
        editButton_->setToolTip(tr("仅可标记为故障或离线；服务端会校验占用状态与当前版本"));
    } else {
        editButton_->setText(tr("编辑"));
        editButton_->setEnabled(hasSelection);
    }
}

void ChargerManagementPage::showAddChargerDialog()
{
    if (realMode_) {
        QMessageBox::information(this, tr("当前不可用"), tr("当前管理契约不支持新增电桩；请通过“新增电站”原子创建电桩。"));
        return;
    }
    showChargerDialog(-1);
}

void ChargerManagementPage::showEditChargerDialog()
{
    if (realMode_) {
        if (selectedRecordIndex_ < 0 || selectedRecordIndex_ >= records_.size()) return;
        const auto& record = records_.at(selectedRecordIndex_);
        bool accepted = false;
        const QString choice = QInputDialog::getItem(this, tr("设置设备状态"),
            tr("仅支持的目标状态："), {tr("故障"), tr("离线")}, 0, false, &accepted);
        if (!accepted) return;
        writeRequestId_ = gateway_->request(QStringLiteral("charger.status"),
            {{QStringLiteral("operationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
             {QStringLiteral("id"), record.serverId}, {QStringLiteral("expectedUpdatedAt"), record.expectedUpdatedAt},
             {QStringLiteral("status"), choice == tr("故障") ? QStringLiteral("FAULT") : QStringLiteral("OFFLINE")}}, this,
            QStringLiteral("charger-write"));
        setFeedback(tr("正在提交电桩状态更新…"));
        return;
    }
    if (selectedRecordIndex_ >= 0 && selectedRecordIndex_ < records_.size()) {
        showChargerDialog(selectedRecordIndex_);
    }
}

void ChargerManagementPage::showChargerDialog(int recordIndex)
{
    const bool isEditing = recordIndex >= 0;
    QDialog dialog(this);
    dialog.setWindowTitle(isEditing ? tr("编辑电桩（Mock）") : tr("新增电桩（Mock）"));
    dialog.setMinimumWidth(440);
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
    auto* stationComboBox = new QComboBox(&dialog);
    stationComboBox->addItems({tr("未来科技城充电站"), tr("滨江智慧园充电站"), tr("城西银泰充电站"),
                               tr("奥体中心充电站"), tr("萧山机场充电站"), tr("富阳智造港充电站")});
    auto* typeComboBox = new QComboBox(&dialog);
    typeComboBox->addItems({tr("直流桩"), tr("交流桩")});
    auto* powerComboBox = new QComboBox(&dialog);
    powerComboBox->addItems({tr("7kW"), tr("60kW"), tr("120kW"), tr("180kW")});
    auto* statusComboBox = new QComboBox(&dialog);
    statusComboBox->addItems({tr("可用"), tr("已预约"), tr("充电中"), tr("离线"), tr("故障")});
    auto* alertLineEdit = new QLineEdit(&dialog);
    alertLineEdit->setPlaceholderText(tr("例如：充电中断；非异常状态留空"));
    for (auto* comboBox : {stationComboBox, typeComboBox, powerComboBox, statusComboBox}) {
        configureManagementComboBox(comboBox);
    }
    if (isEditing) {
        const ChargerRecord& record = records_.at(recordIndex);
        codeLineEdit->setText(record.code);
        codeLineEdit->setReadOnly(true);
        stationComboBox->setCurrentText(record.station);
        typeComboBox->setCurrentText(record.type);
        powerComboBox->setCurrentText(record.power);
        statusComboBox->setCurrentText(record.status);
        alertLineEdit->setText(record.alertType);
    } else {
        codeLineEdit->setPlaceholderText(tr("例如 CP10010642"));
    }
    formLayout->addRow(tr("电桩编号 *"), codeLineEdit);
    formLayout->addRow(tr("所属电站 *"), stationComboBox);
    formLayout->addRow(tr("电桩类型 *"), typeComboBox);
    formLayout->addRow(tr("额定功率 *"), powerComboBox);
    formLayout->addRow(isEditing ? tr("当前状态 *") : tr("初始状态 *"), statusComboBox);
    formLayout->addRow(tr("最近异常"), alertLineEdit);
    layout->addLayout(formLayout);
    auto* hintLabel = createTextLabel(tr("本表单仅维护本地 Mock 数据，不会向设备发送命令或写入数据库。"),
                                      QStringLiteral("color:#718098; font-size:13px;"), &dialog);
    hintLabel->setWordWrap(true);
    layout->addWidget(hintLabel);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(isEditing ? tr("保存修改") : tr("确认新增"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [this, &dialog, codeLineEdit, statusComboBox, alertLineEdit, isEditing]() {
        const QString code = codeLineEdit->text().trimmed();
        if (code.isEmpty()) {
            QMessageBox::warning(&dialog, tr("请补全信息"), tr("请填写电桩编号。"));
            return;
        }
        if (!isEditing) {
            for (const ChargerRecord& record : records_) {
                if (record.code.compare(code, Qt::CaseInsensitive) == 0) {
                    QMessageBox::warning(&dialog, tr("编号重复"), tr("该电桩编号已存在，请使用新的编号。"));
                    return;
                }
            }
        }
        const bool isExceptional = statusComboBox->currentText() == tr("故障")
            || statusComboBox->currentText() == tr("离线");
        if (isExceptional && alertLineEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, tr("请补全异常信息"),
                                 tr("故障或离线电桩需要填写最近异常，便于运营概览与详情一致展示。"));
            return;
        }
        if (!isExceptional && !alertLineEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, tr("异常状态不匹配"),
                                 tr("只有故障或离线电桩可以保留最近异常；请清空异常信息或调整当前状态。"));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QString code = codeLineEdit->text().trimmed();
    if (isEditing) {
        ChargerRecord& record = records_[recordIndex];
        record.station = stationComboBox->currentText();
        record.type = typeComboBox->currentText();
        record.power = powerComboBox->currentText();
        record.status = statusComboBox->currentText();
        record.alertType = alertLineEdit->text().trimmed();
        record.alertOccurredAt = record.alertType.isEmpty() ? QString()
                                                             : tr("2025-06-01 10:30:00");
        record.lastHeartbeat = tr("2025-06-01 10:30:00");
        selectedRecordIndex_ = recordIndex;
        applyFilters();
        showChargerDetails(recordIndex);
        setFeedback(tr("已保存 %1 的本地 Mock 修改").arg(code));
        return;
    }
    records_.append({code, stationComboBox->currentText(), typeComboBox->currentText(), powerComboBox->currentText(),
                     statusComboBox->currentText(), 0, 0, tr("0h 00m"), tr("2025-06-01 10:30:00"),
                     alertLineEdit->text().trimmed(),
                     alertLineEdit->text().trimmed().isEmpty() ? QString()
                                                                : tr("2025-06-01 10:30:00")});
    selectedRecordIndex_ = records_.size() - 1;
    applyFilters();
    showChargerDetails(selectedRecordIndex_);
    setFeedback(tr("已新增 %1（仅本地 Mock）").arg(code));
}

void ChargerManagementPage::refreshSelectedStatus()
{
    if (realMode_) { requestList(); return; }
    if (selectedRecordIndex_ < 0) {
        return;
    }
    records_[selectedRecordIndex_].lastHeartbeat = tr("2025-06-01 10:30:00");
    rebuildTable();
    showChargerDetails(selectedRecordIndex_);
    setFeedback(tr("已刷新 %1 的本地 Mock 状态").arg(records_.at(selectedRecordIndex_).code));
}

void ChargerManagementPage::restartSelectedCharger()
{
    if (selectedRecordIndex_ < 0 || !restartButton_->isEnabled()) {
        return;
    }
    ChargerRecord& record = records_[selectedRecordIndex_];
    const auto choice = QMessageBox::question(
        this, tr("确认远程重启"),
        realMode_ ? tr("确认对电桩 %1 执行受控模拟重启吗？服务端将把允许的状态更新为可用；不会发送真实硬件命令。")
                        .arg(record.code)
                  : tr("确认对电桩 %1 执行远程重启吗？该操作仅更新本地 Mock 状态，不会发送 TCP 命令或写入数据库。")
                        .arg(record.code));
    if (choice != QMessageBox::Yes) {
        return;
    }
    if (realMode_) {
        writeRequestId_ = gateway_->request(QStringLiteral("charger.restart"),
            {{QStringLiteral("operationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
             {QStringLiteral("id"), record.serverId},
             {QStringLiteral("expectedUpdatedAt"), record.expectedUpdatedAt}}, this,
            QStringLiteral("charger-write"));
        setFeedback(tr("正在提交受控模拟重启…"));
        return;
    }
    record.status = tr("可用");
    record.alertType.clear();
    record.alertOccurredAt.clear();
    record.lastHeartbeat = tr("2025-06-01 10:30:00");
    rebuildTable();
    showChargerDetails(selectedRecordIndex_);
    setFeedback(tr("已完成 %1 的本地 Mock 远程重启").arg(record.code));
}

void ChargerManagementPage::clearSelectedAlert()
{
    if (realMode_) return;
    if (selectedRecordIndex_ < 0 || !clearAlertButton_->isEnabled()) {
        return;
    }
    ChargerRecord& record = records_[selectedRecordIndex_];
    const auto choice = QMessageBox::question(
        this, tr("确认解除告警"),
        tr("确认解除 %1 的告警并恢复为可用状态吗？该操作仅更新本地 Mock 状态。").arg(record.code));
    if (choice != QMessageBox::Yes) {
        return;
    }
    record.status = tr("可用");
    record.alertType.clear();
    record.alertOccurredAt.clear();
    rebuildTable();
    showChargerDetails(selectedRecordIndex_);
    setFeedback(tr("已解除 %1 的本地 Mock 告警").arg(record.code));
}

void ChargerManagementPage::showPreviousPage()
{
    if (currentPage_ <= 0) {
        return;
    }
    --currentPage_;
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void ChargerManagementPage::showNextPage()
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

void ChargerManagementPage::setFeedback(const QString& text)
{
    feedbackLabel_->setText(text);
    feedbackLabel_->setToolTip(text);
}

void ChargerManagementPage::setAdminGateway(AdminRequestGateway* gateway)
{
    gateway_ = gateway;
    realMode_ = gateway_ != nullptr;
    if (!gateway_) return;
    powerComboBox_->setEnabled(false);
    powerComboBox_->setToolTip(tr("当前契约不支持按功率筛选"));
    stationComboBox_->setEnabled(false);
    stationComboBox_->setToolTip(tr("当前契约需要站点 ID，列表尚未提供可选项"));
    clearAlertButton_->setEnabled(false);
    clearAlertButton_->setToolTip(tr("当前契约不支持“解除告警”；可使用受控重启恢复为可用"));
    for (auto* button : findChildren<QPushButton*>()) {
        if (button->text() == tr("新增电桩")) {
            button->setEnabled(false);
            button->setToolTip(tr("当前管理员契约不支持此操作"));
        }
    }
    setManagementMetricCardsUnavailable(this, tr("当前契约未提供电桩页汇总指标"));
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

QString ChargerManagementPage::statusCode(const QString& display) const
{
    if (display == tr("可用")) return QStringLiteral("AVAILABLE");
    if (display == tr("已预约")) return QStringLiteral("RESERVED");
    if (display == tr("充电中")) return QStringLiteral("CHARGING");
    if (display == tr("故障")) return QStringLiteral("FAULT");
    if (display == tr("离线")) return QStringLiteral("OFFLINE");
    return {};
}

void ChargerManagementPage::requestList()
{
    if (!gateway_ || !gateway_->isAuthenticated()) return;
    QJsonObject query{{QStringLiteral("page"), currentPage_ + 1},
                      {QStringLiteral("pageSize"), kPageSize},
                      {QStringLiteral("sort"), QStringLiteral("updatedAtDesc")}};
    const QString keyword = keywordLineEdit_->text().trimmed();
    if (!keyword.isEmpty()) query.insert(QStringLiteral("keyword"), keyword);
    if (statusComboBox_->currentText() == tr("异常电桩")) query.insert(QStringLiteral("abnormalOnly"), true);
    else if (const auto status = statusCode(statusComboBox_->currentText()); !status.isEmpty()) query.insert(QStringLiteral("status"), status);
    if (typeComboBox_->currentIndex() > 0) query.insert(QStringLiteral("type"), typeComboBox_->currentIndex() == 1 ? QStringLiteral("FAST") : QStringLiteral("SLOW"));
    listRequestId_ = gateway_->request(QStringLiteral("chargers.list"), query, this, QStringLiteral("charger-list"));
    setFeedback(tr("正在加载服务数据…"));
}

void ChargerManagementPage::handleListResponse(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) {
        records_.clear(); filteredRecordIndexes_.clear(); totalRecords_ = 0; selectedRecordIndex_ = -1; rebuildTable();
        setFeedback(tr("加载失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()));
        return;
    }
    const auto data = response.value(QStringLiteral("data")).toObject();
    records_.clear(); filteredRecordIndexes_.clear(); totalRecords_ = data.value(QStringLiteral("total")).toInt();
    selectedRecordIndex_ = -1;
    for (const auto& value : data.value(QStringLiteral("items")).toArray()) {
        const auto item = value.toObject(); const QString code = item.value(QStringLiteral("status")).toString();
        const QString state = code == QStringLiteral("AVAILABLE") ? tr("可用") : code == QStringLiteral("RESERVED") ? tr("已预约") : code == QStringLiteral("CHARGING") ? tr("充电中") : code == QStringLiteral("FAULT") ? tr("故障") : tr("离线");
        const int seconds = item.value(QStringLiteral("totalChargeSeconds")).toInt();
        records_.append({item.value(QStringLiteral("code")).toString(), item.value(QStringLiteral("stationName")).toString(),
            item.value(QStringLiteral("type")).toString() == QStringLiteral("FAST") ? tr("直流桩") : tr("交流桩"),
            tr("%1 kW").arg(item.value(QStringLiteral("powerWatts")).toInt() / 1000), state, 0,
            item.value(QStringLiteral("totalChargeCount")).toInt(), tr("%1h %2m").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')),
            item.value(QStringLiteral("updatedAt")).toString(), state == tr("故障") || state == tr("离线") ? state : QString(), QString(),
            item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("updatedAt")).toString()});
        filteredRecordIndexes_.append(records_.size() - 1);
    }
    rebuildTable();
    setManagementMetricCardValue(this, 0, tr("%1 台").arg(totalRecords_),
                                 tr("服务端分页总数（当前筛选）"));
    setFeedback(totalRecords_ ? tr("已加载 %1 台电桩（服务端分页）").arg(totalRecords_) : tr("当前没有电桩数据"));
}

void ChargerManagementPage::handleWriteResponse(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) {
        setFeedback(tr("操作未完成：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()));
        return;
    }
    setFeedback(tr("已完成受控模拟重启；正在刷新服务数据"));
    requestList();
}

} // namespace charging::server
