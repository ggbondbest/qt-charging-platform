#include "user_management_page.h"

#include "admin_request_gateway.h"
#include "management_page_widgets.h"
#include "management_time_format.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <limits>

namespace charging::server {

namespace {

constexpr int kPageSize = 10;

QString formatCents(qint64 cents)
{
    return QString::number(cents / 100) + QStringLiteral(".")
        + QStringLiteral("%1").arg(cents % 100, 2, 10, QLatin1Char('0'));
}

bool parseBalanceCents(const QString& text, qint64* cents)
{
    static const QRegularExpression kAmountPattern(
        QStringLiteral("^(0|[1-9][0-9]{0,12})(?:\\.([0-9]{1,2}))?$"));
    const QRegularExpressionMatch match = kAmountPattern.match(text.trimmed());
    if (!match.hasMatch()) {
        return false;
    }

    bool isValid = false;
    const qint64 wholeUnits = match.captured(1).toLongLong(&isValid);
    if (!isValid || wholeUnits > std::numeric_limits<qint64>::max() / 100) {
        return false;
    }
    const QString fraction = match.captured(2).leftJustified(2, QLatin1Char('0'));
    const qint64 fractionalCents = fraction.isEmpty() ? 0 : fraction.toLongLong(&isValid);
    if (!isValid) {
        return false;
    }
    *cents = wholeUnits * 100 + fractionalCents;
    return true;
}

QLabel* createTextLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

QString userStatusStyle(const QString& status)
{
    return status == QObject::tr("正常")
        ? QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:6px; padding:0 7px;"
                         " font-size:12px; font-weight:600;")
        : QStringLiteral("background:#fff0f0; color:#ee5757; border-radius:6px; padding:0 7px;"
                         " font-size:12px; font-weight:600;");
}

QLabel* createStatusTag(const QString& status, QWidget* parent)
{
    auto* label = new QLabel(status, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(userStatusStyle(status));
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

UserManagementPage::UserManagementPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("userManagementPage"));
    setMinimumWidth(kManagementPageMinimumWidth);
    setMinimumHeight(760);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(18);

    auto* metricsLayout = new QHBoxLayout();
    metricsLayout->setSpacing(16);
    metricsLayout->addWidget(createManagementMetricCard(
        tr("用户总数"), tr("36,842"), tr(" 人"), tr("较昨日  +258 (+0.71%)  ↑"), QColor("#347cf6"), 3, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("今日新增用户"), tr("428"), tr(" 人"), tr("较昨日  +36 (+9.18%)  ↑"), QColor("#43c7bc"), 3, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("账户余额总额"), tr("¥ 1,235,600"), QString(), tr("全部用户钱包余额汇总"), QColor("#ff9a26"), 0, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("冻结用户"), tr("243"), tr(" 人"), tr("较昨日  +7 (+2.97%)  ↑"), QColor("#ff5b61"), 0, this));
    layout->addLayout(metricsLayout);

    auto* toolbar = createCompactCard(this);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(18, 12, 18, 12);
    toolbarLayout->setSpacing(10);
    keywordLineEdit_ = new QLineEdit(toolbar);
    keywordLineEdit_->setMinimumWidth(206);
    keywordLineEdit_->setPlaceholderText(tr("⌕  请输入手机号 / 昵称搜索"));
    keywordLineEdit_->setAccessibleName(tr("手机号或昵称关键词"));
    statusComboBox_ = new QComboBox(toolbar);
    statusComboBox_->addItems({tr("用户状态"), tr("正常"), tr("冻结")});
    registrationComboBox_ = new QComboBox(toolbar);
    registrationComboBox_->addItems({tr("注册时间"), tr("近 7 日"), tr("近 30 日"), tr("更早")});
    for (auto* comboBox : {statusComboBox_, registrationComboBox_}) {
        comboBox->setMinimumWidth(126);
        configureManagementComboBox(comboBox);
    }
    minimumBalanceLineEdit_ = new QLineEdit(toolbar);
    minimumBalanceLineEdit_->setPlaceholderText(tr("余额最小值"));
    minimumBalanceLineEdit_->setMaximumWidth(106);
    minimumBalanceLineEdit_->setAccessibleName(tr("账户余额最小值"));
    maximumBalanceLineEdit_ = new QLineEdit(toolbar);
    maximumBalanceLineEdit_->setPlaceholderText(tr("余额最大值"));
    maximumBalanceLineEdit_->setMaximumWidth(106);
    maximumBalanceLineEdit_->setAccessibleName(tr("账户余额最大值"));
    auto* resetButton = new QPushButton(tr("重置"), toolbar);
    resetButton->setObjectName(QStringLiteral("secondaryButton"));
    auto* queryButton = new QPushButton(tr("查询"), toolbar);
    queryButton->setObjectName(QStringLiteral("primaryButton"));
    feedbackLabel_ = createTextLabel(tr("显示全部 36,842 位用户"),
                                     QStringLiteral("color:#6f7d92; font-size:13px;"), toolbar);
    feedbackLabel_->setFixedWidth(180);
    feedbackLabel_->setToolTip(feedbackLabel_->text());
    toolbarLayout->addWidget(keywordLineEdit_, 1);
    toolbarLayout->addWidget(statusComboBox_);
    toolbarLayout->addWidget(registrationComboBox_);
    toolbarLayout->addWidget(minimumBalanceLineEdit_);
    toolbarLayout->addWidget(createTextLabel(tr("~"), QStringLiteral("color:#718098; font-size:15px;"), toolbar));
    toolbarLayout->addWidget(maximumBalanceLineEdit_);
    toolbarLayout->addWidget(feedbackLabel_);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(queryButton);
    layout->addWidget(toolbar);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(kManagementTableMinimumWidth);
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(18, 18, 18, 16);
    tableLayout->setSpacing(12);
    tableTitleLabel_ = createTextLabel(tr("用户列表（共 36,842 人）"),
                                       QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"), tableCard);
    tableLayout->addWidget(tableTitleLabel_);
    tableWidget_ = new QTableWidget(tableCard);
    tableWidget_->setObjectName(QStringLiteral("managementUsersTable"));
    tableWidget_->setColumnCount(8);
    tableWidget_->setHorizontalHeaderLabels(
        {tr("用户ID"), tr("用户昵称"), tr("手机号"), tr("账户余额（元）"), tr("注册时间（北京时间）"),
         tr("累计订单"), tr("状态"), tr("操作")});
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
    tableWidget_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    tableWidget_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Fixed);
    tableWidget_->setColumnWidth(6, kManagementStatusColumnWidth);
    tableWidget_->setColumnWidth(7, 64);
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

    auto* detailCard = createManagementDetailCard(tr("用户详情"), this);
    detailCard->setFixedWidth(kManagementDetailWidth);
    auto* detailLayout = qobject_cast<QVBoxLayout*>(detailCard->layout());
    auto* profileRow = new QHBoxLayout();
    avatarLabel_ = new QLabel(detailCard);
    avatarLabel_->setAlignment(Qt::AlignCenter);
    avatarLabel_->setFixedSize(58, 58);
    avatarLabel_->setStyleSheet(QStringLiteral("background:#d8e9ff; color:#2878d4; border-radius:29px;"
                                               " font-size:22px; font-weight:700;"));
    auto* identityLayout = new QVBoxLayout();
    detailNameLabel_ = createTextLabel(QString(), QStringLiteral("color:#1d2c46; font-size:16px; font-weight:700;"), detailCard);
    detailIdLabel_ = createTextLabel(QString(), QStringLiteral("color:#718098; font-size:13px;"), detailCard);
    detailPhoneLabel_ = createTextLabel(QString(), QStringLiteral("color:#718098; font-size:13px;"), detailCard);
    identityLayout->addWidget(detailNameLabel_);
    identityLayout->addWidget(detailIdLabel_);
    identityLayout->addWidget(detailPhoneLabel_);
    profileRow->addWidget(avatarLabel_);
    profileRow->addLayout(identityLayout, 1);
    detailLayout->addLayout(profileRow);
    detailAccountLabel_ = createTextLabel(QString(), QStringLiteral("color:#55647c; font-size:13px;"), detailCard);
    detailAccountLabel_->setObjectName(QStringLiteral("managementUserDetails"));
    detailAccountLabel_->setWordWrap(true);
    detailLayout->addWidget(detailAccountLabel_);
    auto* riskHeading = new QHBoxLayout();
    riskHeading->addWidget(createTextLabel(tr("风控标签"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    riskHeading->addStretch();
    riskTagLabel_ = createTextLabel(QString(), QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:5px;"
                                                               " padding:4px 7px; font-size:12px; font-weight:600;"), detailCard);
    riskHeading->addWidget(riskTagLabel_);
    detailLayout->addLayout(riskHeading);
    detailLayout->addStretch();
    freezeButton_ = new QPushButton(detailCard);
    freezeButton_->setObjectName(QStringLiteral("primaryButton"));
    riskButton_ = new QPushButton(detailCard);
    riskButton_->setObjectName(QStringLiteral("secondaryButton"));
    detailLayout->addWidget(freezeButton_);
    detailLayout->addWidget(riskButton_);
    contentLayout->addWidget(detailCard);
    layout->addLayout(contentLayout, 1);

    connect(queryButton, &QPushButton::clicked, this, &UserManagementPage::applyFilters);
    connect(statePanel_, &ManagementStatePanel::resetRequested, this,
            &UserManagementPage::resetFilters);
    connect(statePanel_, &ManagementStatePanel::retryRequested, this,
            &UserManagementPage::applyFilters);
    connect(resetButton, &QPushButton::clicked, this, &UserManagementPage::resetFilters);
    connect(keywordLineEdit_, &QLineEdit::returnPressed, this, &UserManagementPage::applyFilters);
    connect(previousPageButton_, &QPushButton::clicked, this, &UserManagementPage::showPreviousPage);
    connect(nextPageButton_, &QPushButton::clicked, this, &UserManagementPage::showNextPage);
    connect(freezeButton_, &QPushButton::clicked, this, &UserManagementPage::toggleSelectedUserStatus);
    connect(riskButton_, &QPushButton::clicked, this, &UserManagementPage::toggleSelectedRiskFocus);
    connect(tableWidget_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr) {
            showUserDetails(item->data(Qt::UserRole).toInt());
        }
    });
    createMockRecords();
    applyFilters();
}

void UserManagementPage::createMockRecords()
{
    records_ = {
        {tr("U10024568"), tr("星辰大海"), tr("138****5678"), 25680, tr("正常"), tr("2025-06-01 10:24:36"), tr("2025-06-01 09:31:27"), 56, false, true},
        {tr("U10024567"), tr("清风明月"), tr("139****2468"), 9850, tr("正常"), tr("2025-05-31 22:17:09"), tr("2025-05-31 18:45:16"), 23, false, true},
        {tr("U10024566"), tr("行云流水"), tr("137****1357"), 0, tr("正常"), tr("2025-05-31 21:03:55"), tr("—"), 0, false, true},
        {tr("U10024565"), tr("小鹿乱撞"), tr("186****8888"), 53260, tr("正常"), tr("2025-05-31 19:40:12"), tr("2025-05-31 17:22:43"), 78, true, true},
        {tr("U10024564"), tr("一路向北"), tr("151****6666"), 3520, tr("冻结"), tr("2025-05-31 18:22:01"), tr("2025-05-30 21:13:09"), 12, false, false},
        {tr("U10024563"), tr("阳光正好"), tr("188****7777"), 12800, tr("正常"), tr("2025-05-31 16:57:39"), tr("2025-06-01 08:12:55"), 34, false, false},
        {tr("U10024562"), tr("未来可期"), tr("199****0000"), 1000, tr("冻结"), tr("2025-05-31 15:36:48"), tr("2025-05-29 11:05:33"), 5, false, false},
        {tr("U10024561"), tr("随遇而安"), tr("136****4321"), 28640, tr("正常"), tr("2025-05-31 14:12:29"), tr("2025-06-01 07:44:12"), 67, false, false},
        {tr("U10024560"), tr("晚风轻拂"), tr("187****5555"), 0, tr("正常"), tr("2025-05-31 12:05:17"), tr("—"), 0, false, false},
        {tr("U10024559"), tr("追风少年"), tr("150****9999"), 7630, tr("正常"), tr("2025-05-31 11:18:44"), tr("2025-05-31 10:02:21"), 9, false, false},
        {tr("U10024558"), tr("春暖花开"), tr("133****2501"), 40600, tr("正常"), tr("2025-05-30 16:03:22"), tr("2025-05-31 13:18:04"), 31, false, false},
        {tr("U10024557"), tr("云卷云舒"), tr("177****1920"), 2360, tr("正常"), tr("2025-05-30 12:36:50"), tr("2025-05-30 18:42:15"), 8, true, false},
    };
}

bool UserManagementPage::recordMatchesFilters(const UserRecord& record) const
{
    const QString keyword = keywordLineEdit_->text().trimmed();
    const bool matchesKeyword = keyword.isEmpty() || record.phone.contains(keyword, Qt::CaseInsensitive)
        || record.nickname.contains(keyword, Qt::CaseInsensitive);
    const bool matchesStatus = statusComboBox_->currentIndex() == 0 || record.status == statusComboBox_->currentText();
    const bool matchesRegistration = registrationComboBox_->currentIndex() == 0
        || registrationComboBox_->currentIndex() == 2 || record.isRecentRegistration;
    bool minimumOk = true;
    bool maximumOk = true;
    qint64 minimumBalanceCents = 0;
    qint64 maximumBalanceCents = std::numeric_limits<qint64>::max();
    if (!minimumBalanceLineEdit_->text().trimmed().isEmpty()) {
        minimumOk = parseBalanceCents(minimumBalanceLineEdit_->text(), &minimumBalanceCents);
    }
    if (!maximumBalanceLineEdit_->text().trimmed().isEmpty()) {
        maximumOk = parseBalanceCents(maximumBalanceLineEdit_->text(), &maximumBalanceCents);
    }
    return minimumOk && maximumOk && minimumBalanceCents <= maximumBalanceCents && matchesKeyword
        && matchesStatus && matchesRegistration && record.balanceCents >= minimumBalanceCents
        && record.balanceCents <= maximumBalanceCents;
}

void UserManagementPage::applyFilters()
{
    if (realMode_) { currentPage_ = 0; requestList(); return; }
    bool minimumOk = true;
    bool maximumOk = true;
    const QString minimumText = minimumBalanceLineEdit_->text().trimmed();
    const QString maximumText = maximumBalanceLineEdit_->text().trimmed();
    qint64 minimumBalanceCents = 0;
    qint64 maximumBalanceCents = std::numeric_limits<qint64>::max();
    if (!minimumText.isEmpty()) {
        minimumOk = parseBalanceCents(minimumText, &minimumBalanceCents);
    }
    if (!maximumText.isEmpty()) {
        maximumOk = parseBalanceCents(maximumText, &maximumBalanceCents);
    }
    if (!minimumOk || !maximumOk || minimumBalanceCents > maximumBalanceCents) {
        setFeedback(tr("余额区间无效：请输入非负金额，且最小值不能大于最大值"), true);
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
    setFeedback(filteredRecordIndexes_.isEmpty() ? tr("未找到符合条件的用户")
                                                  : tr("筛选到 %1 位本地 Mock 用户").arg(filteredRecordIndexes_.size()));
}

void UserManagementPage::resetFilters()
{
    keywordLineEdit_->clear();
    statusComboBox_->setCurrentIndex(0);
    registrationComboBox_->setCurrentIndex(0);
    minimumBalanceLineEdit_->clear();
    maximumBalanceLineEdit_->clear();
    applyFilters();
    if (!realMode_) setFeedback(tr("已重置筛选条件，显示全部本地 Mock 用户"));
}

void UserManagementPage::rebuildTable()
{
    const int pageCount = realMode_ ? qMax(1, (totalRecords_ + kPageSize - 1) / kPageSize)
                                    : qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = realMode_ ? 0 : currentPage_ * kPageSize;
    const int end = realMode_ ? filteredRecordIndexes_.size() : qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const UserRecord& record = records_.at(recordIndex);
        const QList<QString> values = {record.id, record.nickname, record.phone,
                                       formatCents(record.balanceCents), record.registeredAt,
                                       QString::number(record.totalOrders), QString(), QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 6 || column == 7) {
                continue;
            }
            auto* item = createManagementTableItem(values.at(column));
            item->setData(Qt::UserRole, recordIndex);
            item->setTextAlignment(Qt::AlignCenter);
            tableWidget_->setItem(row, column, item);
        }
        tableWidget_->setCellWidget(row, 6, createCompactStatusTag(record.status, tableWidget_));
        auto* detailButton = new QPushButton(tr("详情"), tableWidget_);
        detailButton->setObjectName(QStringLiteral("tableActionButton"));
        detailButton->setAccessibleName(tr("查看 %1 的详情").arg(record.nickname));
        connect(detailButton, &QPushButton::clicked, this, [this, recordIndex]() { showUserDetails(recordIndex); });
        tableWidget_->setCellWidget(row, 7, createManagementTableCell(detailButton, tableWidget_));
    }
    tableTitleLabel_->setText(tr("用户列表（共 %1 人）").arg(realMode_ ? totalRecords_ : filteredRecordIndexes_.size()));
    paginationLabel_->setText(tr("第 %1 / %2 页").arg(currentPage_ + 1).arg(pageCount));
    previousPageButton_->setEnabled(currentPage_ > 0);
    nextPageButton_->setEnabled(currentPage_ + 1 < pageCount);
    updateEmptyState();
    if (!filteredRecordIndexes_.isEmpty()) {
        if (!filteredRecordIndexes_.contains(selectedRecordIndex_)) {
            showUserDetails(filteredRecordIndexes_.first());
        } else {
            updateDetailActions();
        }
    } else {
        selectedRecordIndex_ = -1;
        avatarLabel_->setText(tr("—"));
        detailNameLabel_->setText(tr("暂无匹配用户"));
        detailIdLabel_->clear();
        detailPhoneLabel_->clear();
        detailAccountLabel_->setText(tr("请调整筛选条件后再查看用户画像。"));
        riskTagLabel_->setText(tr("未选择"));
        updateDetailActions();
    }
}

void UserManagementPage::updateEmptyState()
{
    const bool isEmpty = filteredRecordIndexes_.isEmpty();
    const bool hasFilter = !keywordLineEdit_->text().trimmed().isEmpty()
        || statusComboBox_->currentIndex() > 0;
    const auto state = !isEmpty ? ManagementListState::Hidden
        : realMode_ && !hasFilter ? ManagementListState::EmptyInitial
        : ManagementListState::EmptyFiltered;
    statePanel_->setState(state, realMode_ && !hasFilter
                                     ? tr("服务端当前没有用户记录。")
                                     : tr("当前筛选条件下没有用户。请调整条件或点击“重置”。"));
    tableWidget_->setVisible(!isEmpty);
    paginationLabel_->setVisible(!isEmpty);
    previousPageButton_->setVisible(!isEmpty);
    nextPageButton_->setVisible(!isEmpty);
}

void UserManagementPage::showUserDetails(int recordIndex, bool requestDetails)
{
    if (recordIndex < 0 || recordIndex >= records_.size()) {
        return;
    }
    selectedRecordIndex_ = recordIndex;
    const UserRecord& record = records_.at(recordIndex);
    if (realMode_ && gateway_ && requestDetails) {
        detailExpectedServerId_ = record.serverId;
        detailRequestId_ = gateway_->request(QStringLiteral("users.get"), {{QStringLiteral("id"), record.serverId}}, this,
                                             QStringLiteral("user-detail"));
    }
    avatarLabel_->setText(record.nickname.left(1));
    detailNameLabel_->setText(record.nickname + tr("   ·   ") + record.status);
    detailIdLabel_->setText(tr("用户ID：%1").arg(record.id));
    detailPhoneLabel_->setText(tr("手机号：%1").arg(record.phone));
    if (realMode_) {
        detailAccountLabel_->setText(tr("账户余额　¥ %1\n累计订单　%2 笔\n用户状态　%3\n注册时间（北京时间）\n%4\n记录更新时间（北京时间）\n%5")
                                         .arg(formatCents(record.balanceCents)).arg(record.totalOrders)
                                         .arg(record.status, record.registeredAt,
                                              managementBeijingTime(record.expectedUpdatedAt)));
        riskTagLabel_->setText(tr("契约未提供"));
        riskTagLabel_->setStyleSheet(QStringLiteral("background:#f1f4f8; color:#708096; border-radius:5px; padding:4px 7px; font-size:12px; font-weight:600;"));
        updateDetailActions();
        return;
    }
    detailAccountLabel_->setText(
        tr("实名认证　已实名\n累计消费　¥ 2,586.80\n账户余额　¥ %1\n最近登录　2025-06-01 09:58:12\n常用电站　未来科技城充电站、滨江时代广场充电站")
            .arg(formatCents(record.balanceCents)));
    riskTagLabel_->setText(record.isRiskFocused ? tr("风险关注") : tr("信用良好"));
    riskTagLabel_->setStyleSheet(record.isRiskFocused
                                     ? QStringLiteral("background:#fff5e7; color:#ed9b22; border-radius:5px;"
                                                      " padding:4px 7px; font-size:12px; font-weight:600;")
                                     : QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:5px;"
                                                      " padding:4px 7px; font-size:12px; font-weight:600;"));
    updateDetailActions();
    for (int row = 0; row < tableWidget_->rowCount(); ++row) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toInt() == recordIndex) {
            tableWidget_->selectRow(row);
            break;
        }
    }
}

void UserManagementPage::updateDetailActions()
{
    const bool hasSelection = selectedRecordIndex_ >= 0 && selectedRecordIndex_ < records_.size();
    freezeButton_->setEnabled(hasSelection);
    riskButton_->setEnabled(!realMode_ && hasSelection);
    if (!hasSelection) {
        freezeButton_->setText(tr("冻结用户"));
        riskButton_->setText(tr("加入风控关注"));
        return;
    }
    const UserRecord& record = records_.at(selectedRecordIndex_);
    freezeButton_->setText(record.status == tr("冻结") ? tr("解冻用户") : tr("冻结用户"));
    riskButton_->setText(realMode_ ? tr("风控关注（契约未支持）") : (record.isRiskFocused ? tr("移出风控关注") : tr("加入风控关注")));
    if (realMode_) riskButton_->setToolTip(tr("当前管理员契约不支持风控标签写入"));
}

void UserManagementPage::toggleSelectedUserStatus()
{
    if (selectedRecordIndex_ < 0 || selectedRecordIndex_ >= records_.size()) {
        return;
    }
    // QMessageBox has a nested event loop: retain a value snapshot, never a
    // QVector element reference, across the confirmation interaction.
    const int recordIndex = selectedRecordIndex_;
    const UserRecord record = records_.at(recordIndex);
    const bool isFrozen = record.status == tr("冻结");
    const QString action = isFrozen ? tr("解冻") : tr("冻结");
    const auto choice = QMessageBox::question(
        this, tr("确认%1").arg(action),
        realMode_ ? tr("确认要%1用户 %2（手机号后四位 %3）吗？服务端会校验活动订单和当前版本。")
                        .arg(action, record.nickname, record.phone.right(4))
                  : tr("确认要%1用户 %2（手机号后四位 %3）吗？该操作仅更新本地 Mock 状态。")
                        .arg(action, record.nickname, record.phone.right(4)));
    if (choice != QMessageBox::Yes) {
        return;
    }
    if (realMode_) {
        if (!gateway_ || !gateway_->isAuthenticated()) {
            setFeedback(tr("管理员会话已失效，请重新登录后再提交。"), true);
            return;
        }
        writeRequestId_ = gateway_->request(QStringLiteral("user.status"),
            {{QStringLiteral("operationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
             {QStringLiteral("id"), record.serverId}, {QStringLiteral("expectedUpdatedAt"), record.expectedUpdatedAt},
             {QStringLiteral("status"), isFrozen ? QStringLiteral("ACTIVE") : QStringLiteral("FROZEN")}}, this,
            QStringLiteral("user-write"));
        setFeedback(tr("正在提交用户状态更新…")); return;
    }
    auto& currentRecord = records_[recordIndex];
    currentRecord.status = isFrozen ? tr("正常") : tr("冻结");
    applyFilters();
    showUserDetails(recordIndex);
    setFeedback(tr("已%1 %2（仅本地 Mock）").arg(action, record.nickname));
}

void UserManagementPage::toggleSelectedRiskFocus()
{
    if (realMode_) return;
    if (selectedRecordIndex_ < 0) {
        return;
    }
    UserRecord& record = records_[selectedRecordIndex_];
    const QString action = record.isRiskFocused ? tr("移出风控关注") : tr("加入风控关注");
    const auto choice = QMessageBox::question(
        this, tr("确认操作"), tr("确认要将用户 %1 %2 吗？该操作仅更新本地 Mock 状态。")
                                  .arg(record.nickname, action));
    if (choice != QMessageBox::Yes) {
        return;
    }
    record.isRiskFocused = !record.isRiskFocused;
    const int recordIndex = selectedRecordIndex_;
    rebuildTable();
    showUserDetails(recordIndex);
    setFeedback(tr("已%1 %2（仅本地 Mock）").arg(action, record.nickname));
}

void UserManagementPage::showPreviousPage()
{
    if (currentPage_ <= 0) {
        return;
    }
    --currentPage_;
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void UserManagementPage::showNextPage()
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

void UserManagementPage::setFeedback(const QString& text, bool isError)
{
    feedbackLabel_->setText(text);
    feedbackLabel_->setToolTip(text);
    feedbackLabel_->setStyleSheet(isError ? QStringLiteral("color:#ee5757; font-size:13px; font-weight:600;")
                                           : QStringLiteral("color:#6f7d92; font-size:13px;"));
}

void UserManagementPage::setAdminGateway(AdminRequestGateway* gateway)
{
    gateway_ = gateway; realMode_ = gateway_ != nullptr;
    if (!gateway_) return;
    registrationComboBox_->setCurrentIndex(0);
    registrationComboBox_->setEnabled(false);
    registrationComboBox_->setToolTip(tr("当前管理员契约不支持按注册时间筛选"));
    minimumBalanceLineEdit_->clear();
    minimumBalanceLineEdit_->setEnabled(false);
    minimumBalanceLineEdit_->setToolTip(tr("当前管理员契约不支持按余额区间筛选"));
    maximumBalanceLineEdit_->clear();
    maximumBalanceLineEdit_->setEnabled(false);
    maximumBalanceLineEdit_->setToolTip(tr("当前管理员契约不支持按余额区间筛选"));
    setManagementMetricCardsUnavailable(this, tr("当前契约未提供用户页汇总指标"));
    riskButton_->setVisible(false);
    riskTagLabel_->setVisible(false);
    for (auto* label : findChildren<QLabel*>()) {
        if (label->text() == tr("风控状态")) label->setVisible(false);
    }
    connect(gateway_, &AdminRequestGateway::finished, this, [this](const QString& id, const QJsonObject& response) {
        if (id == listRequestId_) handleListResponse(response);
        else if (id == summaryRequestId_) handleSummaryResponse(response);
        else if (id == writeRequestId_) handleWriteResponse(response);
        else if (id == detailRequestId_) handleDetailResponse(response);
    });
    connect(gateway_, &AdminRequestGateway::authenticationChanged, this, [this](bool authenticated) {
        if (!authenticated) { hasRealSnapshot_ = false; }
        else requestList();
    });
    requestList();
}

QString UserManagementPage::statusCode(const QString& display) const
{
    if (display == tr("正常")) return QStringLiteral("ACTIVE");
    if (display == tr("冻结")) return QStringLiteral("FROZEN");
    return {};
}

void UserManagementPage::requestList()
{
    if (!gateway_ || !gateway_->isAuthenticated()) return;
    // Keep confirmed content visible while a newer snapshot is requested.
    if (!hasRealSnapshot_) {
        records_.clear(); filteredRecordIndexes_.clear(); selectedRecordIndex_ = -1;
        totalRecords_ = 0; detailRequestId_.clear(); detailExpectedServerId_.clear(); rebuildTable();
    }
    QJsonObject query{{QStringLiteral("page"), currentPage_ + 1}, {QStringLiteral("pageSize"), kPageSize}, {QStringLiteral("sort"), QStringLiteral("idDesc")}};
    const auto keyword = keywordLineEdit_->text().trimmed(); if (!keyword.isEmpty()) query.insert(QStringLiteral("keyword"), keyword);
    if (const auto status = statusCode(statusComboBox_->currentText()); !status.isEmpty()) query.insert(QStringLiteral("status"), status);
    qint64 minimumBalanceCents = 0;
    qint64 maximumBalanceCents = std::numeric_limits<qint64>::max();
    const QString minimumText = minimumBalanceLineEdit_->text().trimmed();
    const QString maximumText = maximumBalanceLineEdit_->text().trimmed();
    if ((!minimumText.isEmpty() && !parseBalanceCents(minimumText, &minimumBalanceCents)) ||
        (!maximumText.isEmpty() && !parseBalanceCents(maximumText, &maximumBalanceCents)) ||
        minimumBalanceCents > maximumBalanceCents) {
        setFeedback(tr("余额区间无效：请输入非负金额，且最小值不能大于最大值"), true);
        return;
    }
    if (!realMode_ && !minimumText.isEmpty()) query.insert(QStringLiteral("minBalanceCents"), minimumBalanceCents);
    if (!realMode_ && !maximumText.isEmpty()) query.insert(QStringLiteral("maxBalanceCents"), maximumBalanceCents);
    if (!realMode_ && registrationComboBox_->currentIndex() > 0) {
        const QTimeZone zone("Asia/Shanghai");
        const QDate today = QDateTime::currentDateTimeUtc().toTimeZone(zone).date();
        const auto boundary = [&zone](const QDate& date) {
            return QDateTime(date, QTime(0, 0), zone).toUTC().toString(Qt::ISODateWithMs);
        };
        if (registrationComboBox_->currentIndex() == 1) {
            query.insert(QStringLiteral("createdAtFrom"), boundary(today.addDays(-6)));
            query.insert(QStringLiteral("createdAtTo"), boundary(today.addDays(1)));
        } else if (registrationComboBox_->currentIndex() == 2) {
            query.insert(QStringLiteral("createdAtFrom"), boundary(today.addDays(-29)));
            query.insert(QStringLiteral("createdAtTo"), boundary(today.addDays(1)));
        } else {
            query.insert(QStringLiteral("createdAtTo"), boundary(today.addDays(-29)));
        }
    }
    listRequestId_ = gateway_->request(QStringLiteral("users.list"), query, this, QStringLiteral("user-list"));
    query.remove(QStringLiteral("page")); query.remove(QStringLiteral("pageSize")); query.remove(QStringLiteral("sort"));
    summaryRequestId_ = gateway_->request(QStringLiteral("users.summary"), query, this, QStringLiteral("user-summary"));
}

void UserManagementPage::handleDetailResponse(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) {
        setFeedback(tr("详情确认失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()), true);
        return;
    }
    const auto item = response.value(QStringLiteral("data")).toObject().value(QStringLiteral("item")).toObject();
    if (selectedRecordIndex_ < 0 || selectedRecordIndex_ >= records_.size()
        || item.value(QStringLiteral("id")).toString() != detailExpectedServerId_
        || records_.at(selectedRecordIndex_).serverId != detailExpectedServerId_) return;
    auto& record = records_[selectedRecordIndex_];
    record.id = item.value(QStringLiteral("id")).toString(); record.nickname = item.value(QStringLiteral("nickname")).toString(); record.phone = item.value(QStringLiteral("phone")).toString();
    record.balanceCents = item.value(QStringLiteral("balanceCents")).toInteger(); record.status = item.value(QStringLiteral("status")).toString() == QStringLiteral("FROZEN") ? tr("冻结") : tr("正常");
    record.totalOrders = item.value(QStringLiteral("orderCount")).toInt(); record.expectedUpdatedAt = item.value(QStringLiteral("updatedAt")).toString();
    record.registeredAt = managementRegistrationTime(item);
    showUserDetails(selectedRecordIndex_, false);
    detailAccountLabel_->setText(tr("账户余额　¥ %1\n累计订单　%2 笔\n未完成订单　%3 笔\n充值次数　%4 次\n用户状态　%5\n注册时间（北京时间）\n%6\n记录更新时间（北京时间）\n%7")
                                     .arg(formatCents(record.balanceCents)).arg(record.totalOrders)
                                     .arg(item.value(QStringLiteral("unfinishedOrderCount")).toInt())
                                     .arg(item.value(QStringLiteral("rechargeCount")).toInt())
                                     .arg(record.status, record.registeredAt,
                                          managementBeijingTime(record.expectedUpdatedAt)));
}

void UserManagementPage::handleListResponse(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) {
        setFeedback(tr("加载失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()), true); return;
    }
    const QString selectedServerId = selectedRecordIndex_ >= 0 && selectedRecordIndex_ < records_.size()
        ? records_.at(selectedRecordIndex_).serverId : QString();
    records_.clear(); filteredRecordIndexes_.clear(); selectedRecordIndex_ = -1;
    const auto data = response.value(QStringLiteral("data")).toObject(); totalRecords_ = data.value(QStringLiteral("total")).toInt();
    hasRealSnapshot_ = true;
    for (const auto& value : data.value(QStringLiteral("items")).toArray()) {
        const auto item = value.toObject();
        records_.append({item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("nickname")).toString(), item.value(QStringLiteral("phone")).toString(),
            item.value(QStringLiteral("balanceCents")).toInteger(), item.value(QStringLiteral("status")).toString() == QStringLiteral("FROZEN") ? tr("冻结") : tr("正常"),
            managementRegistrationTime(item), tr("契约未提供"), item.value(QStringLiteral("orderCount")).toInt(), false, false,
            item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("updatedAt")).toString()});
        filteredRecordIndexes_.append(records_.size() - 1);
    }
    for (int index = 0; index < records_.size(); ++index) {
        if (records_.at(index).serverId == selectedServerId) { selectedRecordIndex_ = index; break; }
    }
    rebuildTable();
    if (selectedRecordIndex_ >= 0) showUserDetails(selectedRecordIndex_, false);
    setManagementMetricCardValue(this, 0, tr("%1 人").arg(totalRecords_),
                                 tr("服务端分页总数（当前筛选）"));
    setFeedback(totalRecords_ ? tr("已加载 %1 位用户（服务端分页）").arg(totalRecords_) : tr("当前没有用户数据"));
}

void UserManagementPage::handleSummaryResponse(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) return;
    const auto data = response.value(QStringLiteral("data")).toObject();
    const qint64 balanceCents = data.value(QStringLiteral("totalBalanceCents")).toInteger();
    const QString balance = tr("¥ %1.%2").arg(balanceCents / 100).arg(balanceCents % 100, 2, 10, QLatin1Char('0'));
    setManagementMetricCardValue(this, 0, tr("%1 人").arg(data.value(QStringLiteral("totalUsers")).toInteger()), tr("当前筛选范围"));
    setManagementMetricCardValue(this, 1, tr("%1 人").arg(data.value(QStringLiteral("todayNewUsers")).toInteger()), tr("北京时间今日"));
    setManagementMetricCardValue(this, 2, balance, tr("当前筛选范围"));
    setManagementMetricCardValue(this, 3, tr("%1 人").arg(data.value(QStringLiteral("frozenUsers")).toInteger()), tr("当前筛选范围"));
}

void UserManagementPage::handleWriteResponse(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) { setFeedback(tr("操作未完成：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()), true); return; }
    setFeedback(tr("操作已提交，正在刷新服务数据…")); requestList();
}

} // namespace charging::server
