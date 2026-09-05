#include "activity_records_page.h"

#include "admin_request_gateway.h"
#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QtMath>

namespace charging::server {

namespace {

constexpr int kPageSize = 10;

QLabel* createTextLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

QFrame* createCompactCard(QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("contentCard"));
    return card;
}

QString rechargeStatusStyle(const QString& status)
{
    if (status == QObject::tr("成功")) {
        return QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("处理中")) {
        return QStringLiteral("background:#fff3df; color:#f08a1c; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    return QStringLiteral("background:#fff0f0; color:#ee5757; border-radius:6px; padding:0 7px;"
                          " font-size:12px; font-weight:600;");
}

QWidget* createStatusTag(const QString& status, QWidget* parent)
{
    auto* label = new QLabel(status, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setFixedSize(managementStatusTagWidth(status), 26);
    label->setStyleSheet(rechargeStatusStyle(status));
    return createManagementTableCell(label, parent);
}

} // namespace

ActivityRecordsPage::ActivityRecordsPage(ActivityRecordsMode mode, QWidget* parent)
    : QWidget(parent), mode_(mode)
{
    const bool isRecharge = mode_ == ActivityRecordsMode::Recharge;
    setObjectName(isRecharge ? QStringLiteral("rechargeRecordsPage") : QStringLiteral("operationLogPage"));
    setMinimumWidth(kManagementPageMinimumWidth);
    setMinimumHeight(740);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(18);

    auto* metricsLayout = new QHBoxLayout();
    metricsLayout->setSpacing(16);
    if (isRecharge) {
        metricsLayout->addWidget(createManagementMetricCard(tr("今日充值笔数"), tr("286"), tr(" 笔"),
                                                            tr("较昨日  +24 (+9.16%)  ↑"), QColor("#347cf6"), 1, this));
        metricsLayout->addWidget(createManagementMetricCard(tr("今日充值金额"), tr("¥ 18,460"), QString(),
                                                            tr("成功率  98.64%"), QColor("#43c7bc"), 3, this));
        metricsLayout->addWidget(createManagementMetricCard(tr("处理中"), tr("4"), tr(" 笔"),
                                                            tr("需等待支付渠道回调"), QColor("#ff9a26"), 2, this));
        metricsLayout->addWidget(createManagementMetricCard(tr("本月累计"), tr("¥ 428,960"), QString(),
                                                            tr("仅本地 Mock 统计"), QColor("#8a72e8"), 0, this));
    } else {
        metricsLayout->addWidget(createManagementMetricCard(tr("今日操作"), tr("128"), tr(" 次"),
                                                            tr("来自 6 位管理员"), QColor("#347cf6"), 1, this));
        metricsLayout->addWidget(createManagementMetricCard(tr("受控操作"), tr("32"), tr(" 次"),
                                                            tr("冻结、启停、设备处置"), QColor("#ff9a26"), 2, this));
        metricsLayout->addWidget(createManagementMetricCard(tr("执行成功"), tr("126"), tr(" 次"),
                                                            tr("成功率  98.44%"), QColor("#43c7bc"), 3, this));
        metricsLayout->addWidget(createManagementMetricCard(tr("需要关注"), tr("2"), tr(" 条"),
                                                            tr("失败记录可筛选查看"), QColor("#ef6268"), 0, this));
    }
    layout->addLayout(metricsLayout);

    auto* toolbar = createCompactCard(this);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(18, 12, 18, 12);
    toolbarLayout->setSpacing(10);
    keywordLineEdit_ = new QLineEdit(toolbar);
    keywordLineEdit_->setMinimumWidth(210);
    keywordLineEdit_->setPlaceholderText(isRecharge ? tr("⌕  搜索充值单号、用户或手机号")
                                                     : tr("⌕  搜索操作编号、对象或管理员"));
    categoryComboBox_ = new QComboBox(toolbar);
    statusComboBox_ = new QComboBox(toolbar);
    dateRangeComboBox_ = new QComboBox(toolbar);
    if (isRecharge) {
        categoryComboBox_->addItems({tr("充值渠道"), tr("微信支付"), tr("支付宝"), tr("银行卡")});
        statusComboBox_->addItems({tr("充值状态"), tr("成功"), tr("处理中"), tr("失败")});
    } else {
        categoryComboBox_->addItems({tr("操作类型"), tr("冻结用户"), tr("解冻用户"), tr("新增电站"), tr("设备处置")});
        statusComboBox_->addItems({tr("执行结果"), tr("成功"), tr("失败")});
    }
    dateRangeComboBox_->addItems({tr("全部时间"), tr("今日"), tr("近 7 天"), tr("本月")});
    for (auto* comboBox : {categoryComboBox_, statusComboBox_, dateRangeComboBox_}) {
        comboBox->setMinimumWidth(122);
        configureManagementComboBox(comboBox);
    }
    auto* resetButton = new QPushButton(tr("重置"), toolbar);
    resetButton->setObjectName(QStringLiteral("secondaryButton"));
    auto* queryButton = new QPushButton(tr("查询"), toolbar);
    queryButton->setObjectName(QStringLiteral("primaryButton"));
    auto* refreshButton = new QPushButton(tr("手动刷新"), toolbar);
    refreshButton->setObjectName(QStringLiteral("secondaryButton"));
    feedbackLabel_ = createTextLabel(tr("正在显示本地 Mock 数据"),
                                     QStringLiteral("color:#6f7d92; font-size:13px;"), toolbar);
    feedbackLabel_->setFixedWidth(180);
    feedbackLabel_->setToolTip(feedbackLabel_->text());
    toolbarLayout->addWidget(keywordLineEdit_, 1);
    toolbarLayout->addWidget(categoryComboBox_);
    toolbarLayout->addWidget(statusComboBox_);
    toolbarLayout->addWidget(dateRangeComboBox_);
    toolbarLayout->addWidget(feedbackLabel_);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(queryButton);
    toolbarLayout->addWidget(refreshButton);
    layout->addWidget(toolbar);

    auto* bodyLayout = new QHBoxLayout();
    bodyLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(kManagementTableMinimumWidth);
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(18, 18, 18, 16);
    tableLayout->setSpacing(12);
    tableTitleLabel_ = createTextLabel(isRecharge ? tr("充值记录") : tr("操作日志"),
                                       QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"), tableCard);
    tableLayout->addWidget(tableTitleLabel_);
    tableWidget_ = new QTableWidget(tableCard);
    tableWidget_->setColumnCount(isRecharge ? 7 : 7);
    tableWidget_->setHorizontalHeaderLabels(isRecharge
        ? QStringList{tr("充值单号"), tr("用户"), tr("充值金额"), tr("渠道"), tr("状态"), tr("完成时间"), tr("操作")}
        : QStringList{tr("时间"), tr("管理员"), tr("操作类型"), tr("操作对象"), tr("执行结果"), tr("说明"), tr("操作")});
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
    tableWidget_->horizontalHeader()->setSectionResizeMode(isRecharge ? 1 : 5, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    tableWidget_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    tableWidget_->setColumnWidth(4, kManagementStatusColumnWidth);
    tableWidget_->setColumnWidth(6, 72);
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
    bodyLayout->addWidget(tableCard, 1);

    auto* detailCard = createManagementDetailCard(isRecharge ? tr("充值详情") : tr("日志详情"), this);
    detailCard->setFixedWidth(kManagementDetailWidth);
    auto* detailLayout = qobject_cast<QVBoxLayout*>(detailCard->layout());
    detailTitleLabel_ = createTextLabel(QString(), QStringLiteral("color:#273751; font-size:15px; font-weight:700;"), detailCard);
    detailMetaLabel_ = createTextLabel(QString(), QStringLiteral("color:#718098; font-size:13px;"), detailCard);
    detailContentLabel_ = createTextLabel(QString(), QStringLiteral("color:#55647c; font-size:13px;"), detailCard);
    detailMetaLabel_->setWordWrap(true);
    detailContentLabel_->setWordWrap(true);
    detailLayout->addWidget(detailTitleLabel_);
    detailLayout->addWidget(detailMetaLabel_);
    auto* divider = new QFrame(detailCard);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color:#edf1f7;"));
    detailLayout->addWidget(divider);
    detailLayout->addWidget(detailContentLabel_);
    detailLayout->addStretch();
    bodyLayout->addWidget(detailCard);
    layout->addLayout(bodyLayout, 1);

    connect(queryButton, &QPushButton::clicked, this, [this]() { applyFilters(); });
    connect(statePanel_, &ManagementStatePanel::resetRequested, this,
            [this]() { resetFilters(); });
    connect(statePanel_, &ManagementStatePanel::retryRequested, this,
            [this]() { applyFilters(); });
    connect(resetButton, &QPushButton::clicked, this, [this]() { resetFilters(); });
    connect(refreshButton, &QPushButton::clicked, this, [this]() { manualRefresh(); });
    connect(keywordLineEdit_, &QLineEdit::returnPressed, this, [this]() { applyFilters(); });
    connect(previousPageButton_, &QPushButton::clicked, this, [this]() { showPreviousPage(); });
    connect(nextPageButton_, &QPushButton::clicked, this, [this]() { showNextPage(); });
    connect(tableWidget_, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (auto* item = tableWidget_->item(row, 0); item != nullptr) {
            showDetails(item->data(Qt::UserRole).toInt());
        }
    });
    createMockRecords();
    applyFilters();
}

void ActivityRecordsPage::createMockRecords()
{
    if (mode_ == ActivityRecordsMode::Recharge) {
        records_ = {
            {tr("RC202506010001"), tr("2025-06-01 10:26:42"), tr("张先生　138****5678"), tr("微信支付"), tr("¥ 50.00"), tr("成功"), tr("余额已到账，渠道流水号 WX20250601102642。")},
            {tr("RC202506010002"), tr("2025-06-01 10:18:06"), tr("李女士　159****8899"), tr("支付宝"), tr("¥ 100.00"), tr("成功"), tr("余额已到账，渠道流水号 AL20250601101806。")},
            {tr("RC202506010003"), tr("2025-06-01 10:04:17"), tr("王先生　137****1122"), tr("微信支付"), tr("¥ 30.00"), tr("处理中"), tr("等待支付渠道确认；本地 Mock 状态不会影响用户余额。")},
            {tr("RC202506010004"), tr("2025-06-01 09:48:35"), tr("陈女士　186****3344"), tr("银行卡"), tr("¥ 200.00"), tr("失败"), tr("支付渠道拒绝，未写入用户余额。")},
            {tr("RC202506010005"), tr("2025-06-01 09:32:10"), tr("刘先生　152****7788"), tr("支付宝"), tr("¥ 80.00"), tr("成功"), tr("余额已到账。")},
            {tr("RC202506010006"), tr("2025-06-01 09:15:22"), tr("赵先生　139****9900"), tr("微信支付"), tr("¥ 60.00"), tr("成功"), tr("余额已到账。")},
            {tr("RC202506010007"), tr("2025-06-01 09:02:11"), tr("吴女士　158****2211"), tr("银行卡"), tr("¥ 100.00"), tr("成功"), tr("余额已到账。")},
            {tr("RC202506010008"), tr("2025-06-01 08:49:51"), tr("孙先生　187****4455"), tr("微信支付"), tr("¥ 50.00"), tr("成功"), tr("余额已到账。")},
            {tr("RC202506010009"), tr("2025-06-01 08:37:49"), tr("周女士　150****6677"), tr("支付宝"), tr("¥ 20.00"), tr("成功"), tr("余额已到账。")},
            {tr("RC202506010010"), tr("2025-06-01 08:22:03"), tr("黄先生　188****5566"), tr("微信支付"), tr("¥ 100.00"), tr("成功"), tr("余额已到账。")},
            {tr("RC202505310011"), tr("2025-05-31 23:43:28"), tr("杨女士　136****3456"), tr("支付宝"), tr("¥ 30.00"), tr("成功"), tr("余额已到账。")},
            {tr("RC202505310012"), tr("2025-05-31 22:20:16"), tr("何先生　131****8024"), tr("微信支付"), tr("¥ 50.00"), tr("成功"), tr("余额已到账。")},
        };
        return;
    }
    records_ = {
        {tr("LOG-20250601-001"), tr("2025-06-01 10:25:10"), tr("管理员 A"), tr("冻结用户"), tr("用户 U10024564"), tr("成功"), tr("冻结用户 一路向北（手机号后四位 6666），已请求写入操作审计。")},
        {tr("LOG-20250601-002"), tr("2025-06-01 10:18:44"), tr("管理员 A"), tr("设备处置"), tr("电桩 CP10010267"), tr("成功"), tr("已确认故障并更新本地 Mock 展示状态。")},
        {tr("LOG-20250601-003"), tr("2025-06-01 10:03:22"), tr("管理员 B"), tr("新增电站"), tr("电站 STN000337"), tr("成功"), tr("新增西湖文体中心充电站；真实阶段需由 Service 原子写入。")},
        {tr("LOG-20250601-004"), tr("2025-06-01 09:56:08"), tr("管理员 A"), tr("设备处置"), tr("电桩 CP10010086"), tr("失败"), tr("设备处于充电中，按 Mock 规则拒绝远程重启。")},
        {tr("LOG-20250601-005"), tr("2025-06-01 09:40:17"), tr("管理员 C"), tr("解冻用户"), tr("用户 U10024562"), tr("成功"), tr("已在本地 Mock 中恢复正常账户状态。")},
        {tr("LOG-20250601-006"), tr("2025-06-01 09:24:31"), tr("管理员 B"), tr("新增电站"), tr("电站 STN000336"), tr("成功"), tr("已完成表单校验并刷新本地 Mock 列表。")},
        {tr("LOG-20250601-007"), tr("2025-06-01 09:11:06"), tr("管理员 A"), tr("设备处置"), tr("电桩 CP10010533"), tr("成功"), tr("已解除本地 Mock 告警并刷新详情。")},
        {tr("LOG-20250601-008"), tr("2025-06-01 08:48:51"), tr("管理员 D"), tr("冻结用户"), tr("用户 U10024559"), tr("成功"), tr("已在本地 Mock 中标记账户冻结。")},
        {tr("LOG-20250601-009"), tr("2025-06-01 08:36:00"), tr("管理员 C"), tr("新增电站"), tr("电站 STN000335"), tr("成功"), tr("新增操作仅用于演示，不写入数据库。")},
        {tr("LOG-20250601-010"), tr("2025-06-01 08:18:47"), tr("管理员 B"), tr("设备处置"), tr("电桩 CP10010495"), tr("成功"), tr("已刷新本地 Mock 电桩状态。")},
        {tr("LOG-20250531-011"), tr("2025-05-31 23:38:12"), tr("管理员 A"), tr("解冻用户"), tr("用户 U10024542"), tr("成功"), tr("已在本地 Mock 中解除冻结。")},
        {tr("LOG-20250531-012"), tr("2025-05-31 22:15:09"), tr("管理员 D"), tr("设备处置"), tr("电桩 CP10010378"), tr("成功"), tr("已查看本地 Mock 告警详情。")},
    };
}

bool ActivityRecordsPage::matchesFilters(const Record& record) const
{
    const QString keyword = keywordLineEdit_->text().trimmed();
    const bool matchesKeyword = keyword.isEmpty() || record.id.contains(keyword, Qt::CaseInsensitive)
        || record.subject.contains(keyword, Qt::CaseInsensitive) || record.amountOrTarget.contains(keyword, Qt::CaseInsensitive);
    const bool matchesCategory = categoryComboBox_->currentIndex() == 0
        || record.category == categoryComboBox_->currentText();
    const bool matchesStatus = statusComboBox_->currentIndex() == 0
        || record.status == statusComboBox_->currentText();
    // The deterministic sample covers the current day and its preceding day.
    // "近 7 天" keeps both; the current-day and current-month filters show June only.
    const int dateRangeIndex = dateRangeComboBox_->currentIndex();
    const bool matchesDate = dateRangeIndex == 0 || dateRangeIndex == 2
        || record.occurredAt.startsWith(QStringLiteral("2025-06-01"));
    return matchesKeyword && matchesCategory && matchesStatus && matchesDate;
}

void ActivityRecordsPage::applyFilters()
{
    if (realMode_) { currentPage_ = 0; requestList(); return; }
    filteredRecordIndexes_.clear();
    for (int index = 0; index < records_.size(); ++index) {
        if (matchesFilters(records_.at(index))) {
            filteredRecordIndexes_.append(index);
        }
    }
    currentPage_ = 0;
    rebuildTable();
    setFeedback(filteredRecordIndexes_.isEmpty() ? tr("未找到符合条件的本地 Mock 记录")
                                                  : tr("筛选到 %1 条本地 Mock 记录").arg(filteredRecordIndexes_.size()));
}

void ActivityRecordsPage::resetFilters()
{
    keywordLineEdit_->clear();
    categoryComboBox_->setCurrentIndex(0);
    statusComboBox_->setCurrentIndex(0);
    dateRangeComboBox_->setCurrentIndex(0);
    applyFilters();
    if (!realMode_) setFeedback(tr("已重置筛选条件，显示全部本地 Mock 记录"));
}

void ActivityRecordsPage::rebuildTable()
{
    const bool isRecharge = mode_ == ActivityRecordsMode::Recharge;
    const int pageCount = realMode_ ? qMax(1, (totalRecords_ + kPageSize - 1) / kPageSize)
                                    : qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = realMode_ ? 0 : currentPage_ * kPageSize;
    const int end = realMode_ ? filteredRecordIndexes_.size() : qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const Record& record = records_.at(recordIndex);
        const QStringList values = isRecharge
            ? QStringList{record.id, record.subject, record.amountOrTarget, record.category, QString(), record.occurredAt, QString()}
            : QStringList{record.occurredAt, record.subject, record.category, record.amountOrTarget, QString(), record.details, QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 4 || column == 6) {
                continue;
            }
            auto* item = createManagementTableItem(values.at(column));
            item->setData(Qt::UserRole, recordIndex);
            item->setTextAlignment(Qt::AlignCenter);
            tableWidget_->setItem(row, column, item);
        }
        tableWidget_->setCellWidget(row, 4, createStatusTag(record.status, tableWidget_));
        auto* detailButton = new QPushButton(tr("详情"), tableWidget_);
        detailButton->setObjectName(QStringLiteral("tableActionButton"));
        connect(detailButton, &QPushButton::clicked, this, [this, recordIndex]() { showDetails(recordIndex); });
        tableWidget_->setCellWidget(row, 6, createManagementTableCell(detailButton, tableWidget_));
    }
    tableTitleLabel_->setText((isRecharge ? tr("充值记录") : tr("操作日志"))
                              + tr("（共 %1 条）").arg(realMode_ ? totalRecords_ : filteredRecordIndexes_.size()));
    paginationLabel_->setText(tr("第 %1 / %2 页").arg(currentPage_ + 1).arg(pageCount));
    previousPageButton_->setEnabled(currentPage_ > 0);
    nextPageButton_->setEnabled(currentPage_ + 1 < pageCount);
    const bool isEmpty = filteredRecordIndexes_.isEmpty();
    tableWidget_->setVisible(!isEmpty);
    const bool hasFilter = !keywordLineEdit_->text().trimmed().isEmpty()
        || statusComboBox_->currentIndex() > 0 || dateRangeComboBox_->currentIndex() > 0;
    const auto state = !isEmpty ? ManagementListState::Hidden
        : realMode_ && !hasFilter ? ManagementListState::EmptyInitial
        : ManagementListState::EmptyFiltered;
    statePanel_->setState(state, realMode_ && !hasFilter
                                     ? (isRecharge ? tr("服务端当前没有充值记录。")
                                                   : tr("服务端当前没有操作日志；完成受控写操作后会记录在这里。"))
                                     : (isRecharge ? tr("当前筛选条件下没有充值记录。请调整条件或点击“重置”。")
                                                   : tr("当前筛选条件下没有操作日志。请调整条件或点击“重置”。")));
    paginationLabel_->setVisible(!isEmpty);
    previousPageButton_->setVisible(!isEmpty);
    nextPageButton_->setVisible(!isEmpty);
    if (isEmpty) {
        selectedRecordIndex_ = -1;
        detailTitleLabel_->setText(tr("暂无匹配记录"));
        detailMetaLabel_->setText(tr("请调整筛选条件后再查看详情。"));
        detailContentLabel_->clear();
    } else if (!filteredRecordIndexes_.contains(selectedRecordIndex_)) {
        showDetails(filteredRecordIndexes_.first());
    }
}

void ActivityRecordsPage::showDetails(int recordIndex)
{
    if (recordIndex < 0 || recordIndex >= records_.size()) {
        return;
    }
    selectedRecordIndex_ = recordIndex;
    const Record& record = records_.at(recordIndex);
    if (realMode_ && gateway_) {
        detailRequestId_ = gateway_->request(mode_ == ActivityRecordsMode::Recharge ? QStringLiteral("recharges.get") : QStringLiteral("operation_logs.get"),
                                             {{QStringLiteral("id"), record.serverId}}, this, QStringLiteral("activity-detail"));
    }
    const bool isRecharge = mode_ == ActivityRecordsMode::Recharge;
    detailTitleLabel_->setText(record.id);
    detailMetaLabel_->setText(isRecharge ? tr("完成时间：%1\n用户：%2").arg(record.occurredAt, record.subject)
                                         : tr("发生时间：%1\n执行人：%2").arg(record.occurredAt, record.subject));
    if (realMode_) {
        detailContentLabel_->setText(isRecharge
            ? tr("充值金额　%1\n充值渠道　契约未提供\n处理状态　%2\n\n%3")
                  .arg(record.amountOrTarget, record.status, record.details)
            : tr("操作类型　%1\n操作对象　%2\n执行结果　契约未提供\n\n%3")
                  .arg(record.category, record.amountOrTarget, record.details));
    } else {
        detailContentLabel_->setText(isRecharge
            ? tr("充值金额　%1\n充值渠道　%2\n处理状态　%3\n\n%4\n\n仅本地 Mock 展示。")
                  .arg(record.amountOrTarget, record.category, record.status, record.details)
            : tr("操作类型　%1\n操作对象　%2\n执行结果　%3\n\n%4\n\n仅本地 Mock 展示。")
                  .arg(record.category, record.amountOrTarget, record.status, record.details));
    }
    for (int row = 0; row < tableWidget_->rowCount(); ++row) {
        if (auto* item = tableWidget_->item(row, 0);
            item != nullptr && item->data(Qt::UserRole).toInt() == recordIndex) {
            tableWidget_->selectRow(row);
            break;
        }
    }
}

void ActivityRecordsPage::showPreviousPage()
{
    if (currentPage_ <= 0) {
        return;
    }
    --currentPage_;
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void ActivityRecordsPage::showNextPage()
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

void ActivityRecordsPage::manualRefresh()
{
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已于 2025-06-01 10:30:00 刷新本地 Mock 数据；真实记录需等待 Service 返回。"));
}

void ActivityRecordsPage::setFeedback(const QString& text, bool isError)
{
    feedbackLabel_->setText(text);
    feedbackLabel_->setToolTip(text);
    feedbackLabel_->setStyleSheet(QStringLiteral("color:%1; font-size:13px;").arg(isError ? QStringLiteral("#d84a4a") : QStringLiteral("#6f7d92")));
}

void ActivityRecordsPage::setAdminGateway(AdminRequestGateway* gateway)
{
    gateway_ = gateway; realMode_ = gateway_ != nullptr;
    if (!gateway_) return;
    if (mode_ == ActivityRecordsMode::Recharge) {
        categoryComboBox_->setEnabled(false); categoryComboBox_->setToolTip(tr("当前契约不返回充值渠道"));
    } else {
        categoryComboBox_->setEnabled(false); categoryComboBox_->setToolTip(tr("当前操作类型下拉项不是契约枚举；请使用关键字查询"));
        statusComboBox_->setEnabled(false); statusComboBox_->setToolTip(tr("操作日志只读且不提供执行结果字段"));
    }
    setManagementMetricCardsUnavailable(
        this, tr("当前契约未提供%1页汇总指标")
                  .arg(mode_ == ActivityRecordsMode::Recharge ? tr("充值记录") : tr("操作日志")));
    connect(gateway_, &AdminRequestGateway::finished, this, [this](const QString& id, const QJsonObject& response) {
        if (id == listRequestId_) handleListResponse(response);
        else if (id == detailRequestId_ && !response.value(QStringLiteral("success")).toBool())
            setFeedback(tr("详情确认失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()), true);
    });
    connect(gateway_, &AdminRequestGateway::authenticationChanged, this, [this](bool authenticated) {
        if (authenticated) requestList();
    });
    requestList();
}

void ActivityRecordsPage::requestList()
{
    if (!gateway_ || !gateway_->isAuthenticated()) return;
    const bool recharge = mode_ == ActivityRecordsMode::Recharge;
    QJsonObject query{{QStringLiteral("page"), currentPage_ + 1}, {QStringLiteral("pageSize"), kPageSize},
                      {QStringLiteral("sort"), QStringLiteral("createdAtDesc")}};
    const auto keyword = keywordLineEdit_->text().trimmed(); if (!keyword.isEmpty()) query.insert(QStringLiteral("keyword"), keyword);
    if (recharge && statusComboBox_->currentIndex() == 1) query.insert(QStringLiteral("status"), QStringLiteral("SUCCESS"));
    if (recharge && statusComboBox_->currentIndex() == 3) query.insert(QStringLiteral("status"), QStringLiteral("FAILED"));
    if (dateRangeComboBox_->currentIndex() > 0) {
        const auto now = QDateTime::currentDateTimeUtc(); QDate from = now.date();
        if (dateRangeComboBox_->currentIndex() == 2) from = from.addDays(-6);
        else if (dateRangeComboBox_->currentIndex() == 3) from = QDate(from.year(), from.month(), 1);
        query.insert(QStringLiteral("createdAtFrom"), QDateTime(from, QTime(0,0), Qt::UTC).toString(Qt::ISODateWithMs));
        query.insert(QStringLiteral("createdAtTo"), QDateTime(now.date().addDays(1), QTime(0,0), Qt::UTC).toString(Qt::ISODateWithMs));
    }
    listRequestId_ = gateway_->request(recharge ? QStringLiteral("recharges.list") : QStringLiteral("operation_logs.list"), query, this,
                                       recharge ? QStringLiteral("recharge-list") : QStringLiteral("operation-log-list"));
    setFeedback(tr("正在加载服务数据…"));
}

void ActivityRecordsPage::handleListResponse(const QJsonObject& response)
{
    records_.clear(); filteredRecordIndexes_.clear(); selectedRecordIndex_ = -1;
    if (!response.value(QStringLiteral("success")).toBool()) { totalRecords_ = 0; rebuildTable(); setFeedback(tr("加载失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()), true); return; }
    const auto data = response.value(QStringLiteral("data")).toObject(); totalRecords_ = data.value(QStringLiteral("total")).toInt();
    const bool recharge = mode_ == ActivityRecordsMode::Recharge;
    for (const auto& value : data.value(QStringLiteral("items")).toArray()) {
        const auto i = value.toObject();
        if (recharge) {
            const bool success = i.value(QStringLiteral("status")).toString() == QStringLiteral("SUCCESS");
            records_.append({i.value(QStringLiteral("transactionNo")).toString(), i.value(QStringLiteral("createdAt")).toString(),
                i.value(QStringLiteral("nickname")).toString() + tr("　") + i.value(QStringLiteral("phone")).toString(), tr("契约未提供"),
                tr("¥ %1").arg(QString::number(i.value(QStringLiteral("amountCents")).toInteger() / 100.0, 'f', 2)), success ? tr("成功") : tr("失败"),
                tr("余额变更后余额：¥ %1").arg(QString::number(i.value(QStringLiteral("balanceAfterCents")).toInteger() / 100.0, 'f', 2)), i.value(QStringLiteral("id")).toString()});
        } else {
            records_.append({i.value(QStringLiteral("id")).toString(), i.value(QStringLiteral("createdAt")).toString(),
                i.value(QStringLiteral("adminId")).isNull() ? tr("系统") : tr("管理员 ID：%1").arg(i.value(QStringLiteral("adminId")).toString()),
                i.value(QStringLiteral("action")).toString(), i.value(QStringLiteral("targetType")).toString() + tr("：") + i.value(QStringLiteral("targetId")).toString(),
                tr("—"), tr("审计日志仅返回安全元数据；不暴露 details_json。"), i.value(QStringLiteral("id")).toString()});
        }
        filteredRecordIndexes_.append(records_.size() - 1);
    }
    rebuildTable(); setFeedback(totalRecords_ ? tr("已加载 %1 条记录（服务端分页）").arg(totalRecords_) : tr("当前没有记录"));
}

} // namespace charging::server
