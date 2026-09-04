#include "user_management_page.h"

#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
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
    label->setFixedSize(46, 26);
    return createManagementTableCell(label, parent);
}

QFrame* createCompactCard(QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("contentCard"));
    return card;
}

class UserGrowthTrendWidget final : public QWidget
{
public:
    explicit UserGrowthTrendWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(134);
        setAccessibleName(QObject::tr("近七日用户增长趋势图"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF chart = rect().adjusted(22, 12, -8, -26);
        painter.setPen(QPen(QColor("#e9eef5"), 1));
        for (int row = 0; row < 3; ++row) {
            const qreal y = chart.top() + chart.height() * row / 2.0;
            painter.drawLine(QPointF(chart.left(), y), QPointF(chart.right(), y));
        }
        const QList<qreal> values = {0.18, 0.23, 0.34, 0.39, 0.66, 0.71, 0.78};
        QPainterPath line;
        QPainterPath area;
        for (int index = 0; index < values.size(); ++index) {
            const qreal x = chart.left() + chart.width() * index / (values.size() - 1.0);
            const qreal y = chart.bottom() - chart.height() * values.at(index);
            if (index == 0) {
                line.moveTo(x, y);
                area.moveTo(x, chart.bottom());
                area.lineTo(x, y);
            } else {
                line.lineTo(x, y);
                area.lineTo(x, y);
            }
        }
        area.lineTo(chart.right(), chart.bottom());
        area.closeSubpath();
        QLinearGradient fill(chart.topLeft(), chart.bottomLeft());
        fill.setColorAt(0, QColor(40, 120, 240, 60));
        fill.setColorAt(1, QColor(40, 120, 240, 2));
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(area);
        painter.setPen(QPen(QColor("#2878f0"), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(line);
        painter.setBrush(QColor("#2878f0"));
        painter.setPen(Qt::NoPen);
        for (int index = 0; index < values.size(); ++index) {
            const qreal x = chart.left() + chart.width() * index / (values.size() - 1.0);
            const qreal y = chart.bottom() - chart.height() * values.at(index);
            painter.drawEllipse(QPointF(x, y), 3, 3);
        }
        painter.setPen(QColor("#8795a9"));
        QFont font = painter.font();
        font.setPixelSize(10);
        painter.setFont(font);
        const QStringList dates = {QStringLiteral("05-26"), QStringLiteral("05-27"), QStringLiteral("05-28"),
                                   QStringLiteral("05-29"), QStringLiteral("05-30"), QStringLiteral("05-31"),
                                   QStringLiteral("06-01")};
        for (int index = 0; index < dates.size(); ++index) {
            const qreal x = chart.left() + chart.width() * index / (dates.size() - 1.0);
            painter.drawText(QRectF(x - 16, chart.bottom() + 6, 32, 12), Qt::AlignCenter, dates.at(index));
        }
    }
};

} // namespace

UserManagementPage::UserManagementPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("userManagementPage"));
    setMinimumWidth(1120);
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
        tr("活跃用户（7日）"), tr("12,356"), tr(" 人"), tr("较昨日  +412 (+3.45%)  ↑"), QColor("#ff9a26"), 0, this));
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
    feedbackLabel_->setMinimumWidth(142);
    toolbarLayout->addWidget(keywordLineEdit_, 1);
    toolbarLayout->addWidget(statusComboBox_);
    toolbarLayout->addWidget(registrationComboBox_);
    toolbarLayout->addWidget(minimumBalanceLineEdit_);
    toolbarLayout->addWidget(createTextLabel(tr("~"), QStringLiteral("color:#718098; font-size:15px;"), toolbar));
    toolbarLayout->addWidget(maximumBalanceLineEdit_);
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(queryButton);
    toolbarLayout->addWidget(feedbackLabel_);
    layout->addWidget(toolbar);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(730);
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(18, 18, 18, 16);
    tableLayout->setSpacing(12);
    tableTitleLabel_ = createTextLabel(tr("用户列表（共 36,842 人）"),
                                       QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"), tableCard);
    tableLayout->addWidget(tableTitleLabel_);
    tableWidget_ = new QTableWidget(tableCard);
    tableWidget_->setColumnCount(10);
    tableWidget_->setHorizontalHeaderLabels(
        {tr("用户ID"), tr("用户昵称"), tr("手机号"), tr("账户余额（元）"), tr("注册时间"),
         tr("最近充电时间"), tr("累计订单"), tr("状态"), tr("风控操作"), tr("操作")});
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
    tableWidget_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Fixed);
    tableWidget_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Fixed);
    tableWidget_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Fixed);
    tableWidget_->setColumnWidth(7, 60);
    tableWidget_->setColumnWidth(8, 68);
    tableWidget_->setColumnWidth(9, 64);
    tableLayout->addWidget(tableWidget_, 1);
    emptyStateLabel_ = createTextLabel(tr("当前筛选条件下没有用户。请调整条件或点击“重置”。"),
                                       QStringLiteral("color:#6f7d92; min-height:92px; font-size:14px;"), tableCard);
    emptyStateLabel_->setAlignment(Qt::AlignCenter);
    tableLayout->addWidget(emptyStateLabel_);
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
    detailCard->setMinimumWidth(292);
    detailCard->setMaximumWidth(318);
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
    detailAccountLabel_->setWordWrap(true);
    detailLayout->addWidget(detailAccountLabel_);
    auto* riskHeading = new QHBoxLayout();
    riskHeading->addWidget(createTextLabel(tr("风控标签"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    riskHeading->addStretch();
    riskTagLabel_ = createTextLabel(QString(), QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:5px;"
                                                               " padding:4px 7px; font-size:12px; font-weight:600;"), detailCard);
    riskHeading->addWidget(riskTagLabel_);
    detailLayout->addLayout(riskHeading);
    detailLayout->addWidget(createTextLabel(tr("用户增长趋势（近 7 日）"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    detailLayout->addWidget(new UserGrowthTrendWidget(detailCard));
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
        {tr("U10024568"), tr("星辰大海"), tr("138****5678"), 256.80, tr("正常"), tr("2025-06-01 10:24:36"), tr("2025-06-01 09:31:27"), 56, false, true},
        {tr("U10024567"), tr("清风明月"), tr("139****2468"), 98.50, tr("正常"), tr("2025-05-31 22:17:09"), tr("2025-05-31 18:45:16"), 23, false, true},
        {tr("U10024566"), tr("行云流水"), tr("137****1357"), 0.00, tr("正常"), tr("2025-05-31 21:03:55"), tr("—"), 0, false, true},
        {tr("U10024565"), tr("小鹿乱撞"), tr("186****8888"), 532.60, tr("正常"), tr("2025-05-31 19:40:12"), tr("2025-05-31 17:22:43"), 78, true, true},
        {tr("U10024564"), tr("一路向北"), tr("151****6666"), -35.20, tr("冻结"), tr("2025-05-31 18:22:01"), tr("2025-05-30 21:13:09"), 12, false, false},
        {tr("U10024563"), tr("阳光正好"), tr("188****7777"), 128.00, tr("正常"), tr("2025-05-31 16:57:39"), tr("2025-06-01 08:12:55"), 34, false, false},
        {tr("U10024562"), tr("未来可期"), tr("199****0000"), 10.00, tr("冻结"), tr("2025-05-31 15:36:48"), tr("2025-05-29 11:05:33"), 5, false, false},
        {tr("U10024561"), tr("随遇而安"), tr("136****4321"), 286.40, tr("正常"), tr("2025-05-31 14:12:29"), tr("2025-06-01 07:44:12"), 67, false, false},
        {tr("U10024560"), tr("晚风轻拂"), tr("187****5555"), 0.00, tr("正常"), tr("2025-05-31 12:05:17"), tr("—"), 0, false, false},
        {tr("U10024559"), tr("追风少年"), tr("150****9999"), 76.30, tr("正常"), tr("2025-05-31 11:18:44"), tr("2025-05-31 10:02:21"), 9, false, false},
        {tr("U10024558"), tr("春暖花开"), tr("133****2501"), 406.00, tr("正常"), tr("2025-05-30 16:03:22"), tr("2025-05-31 13:18:04"), 31, false, false},
        {tr("U10024557"), tr("云卷云舒"), tr("177****1920"), 23.60, tr("正常"), tr("2025-05-30 12:36:50"), tr("2025-05-30 18:42:15"), 8, true, false},
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
    const double minimumBalance = minimumBalanceLineEdit_->text().trimmed().isEmpty()
        ? -1.0e12 : minimumBalanceLineEdit_->text().toDouble(&minimumOk);
    const double maximumBalance = maximumBalanceLineEdit_->text().trimmed().isEmpty()
        ? 1.0e12 : maximumBalanceLineEdit_->text().toDouble(&maximumOk);
    return minimumOk && maximumOk && minimumBalance <= maximumBalance && matchesKeyword && matchesStatus
        && matchesRegistration && record.balance >= minimumBalance && record.balance <= maximumBalance;
}

void UserManagementPage::applyFilters()
{
    bool minimumOk = true;
    bool maximumOk = true;
    const QString minimumText = minimumBalanceLineEdit_->text().trimmed();
    const QString maximumText = maximumBalanceLineEdit_->text().trimmed();
    const double minimumBalance = minimumText.isEmpty() ? -1.0e12 : minimumText.toDouble(&minimumOk);
    const double maximumBalance = maximumText.isEmpty() ? 1.0e12 : maximumText.toDouble(&maximumOk);
    if (!minimumOk || !maximumOk || minimumBalance > maximumBalance) {
        setFeedback(tr("余额区间无效：请输入数字，且最小值不能大于最大值"), true);
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
    setFeedback(tr("已重置筛选条件，显示全部本地 Mock 用户"));
}

void UserManagementPage::rebuildTable()
{
    const int pageCount = qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = currentPage_ * kPageSize;
    const int end = qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const UserRecord& record = records_.at(recordIndex);
        const QList<QString> values = {record.id, record.nickname, record.phone,
                                       QString::number(record.balance, 'f', 2), record.registeredAt,
                                       record.lastChargeAt, QString::number(record.totalOrders), QString(), QString(), QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 7 || column == 8 || column == 9) {
                continue;
            }
            auto* item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, recordIndex);
            item->setTextAlignment(Qt::AlignCenter);
            if (column == 3 && record.balance < 0) {
                item->setForeground(QColor("#ee5757"));
            }
            tableWidget_->setItem(row, column, item);
        }
        tableWidget_->setCellWidget(row, 7, createCompactStatusTag(record.status, tableWidget_));
        auto* riskButton = new QPushButton(record.isRiskFocused ? tr("已关注") : tr("关注"), tableWidget_);
        riskButton->setObjectName(QStringLiteral("tableActionButton"));
        riskButton->setAccessibleName(tr("切换 %1 的风控关注").arg(record.nickname));
        riskButton->setStyleSheet(record.isRiskFocused
                                      ? QStringLiteral("QPushButton { background:#fff5e7; color:#ed9b22; border:0;"
                                                       " border-radius:6px; min-height:26px; max-height:26px;"
                                                       " min-width:0; padding:0 8px; font-size:12px; }")
                                      : QString());
        connect(riskButton, &QPushButton::clicked, this, [this, recordIndex]() {
            showUserDetails(recordIndex);
            toggleSelectedRiskFocus();
        });
        tableWidget_->setCellWidget(row, 8, createManagementTableCell(riskButton, tableWidget_));
        auto* detailButton = new QPushButton(tr("详情"), tableWidget_);
        detailButton->setObjectName(QStringLiteral("tableActionButton"));
        detailButton->setAccessibleName(tr("查看 %1 的详情").arg(record.nickname));
        connect(detailButton, &QPushButton::clicked, this, [this, recordIndex]() { showUserDetails(recordIndex); });
        tableWidget_->setCellWidget(row, 9, createManagementTableCell(detailButton, tableWidget_));
    }
    tableTitleLabel_->setText(tr("用户列表（共 %1 人）").arg(filteredRecordIndexes_.size()));
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
    emptyStateLabel_->setVisible(isEmpty);
    tableWidget_->setVisible(!isEmpty);
    paginationLabel_->setVisible(!isEmpty);
    previousPageButton_->setVisible(!isEmpty);
    nextPageButton_->setVisible(!isEmpty);
}

void UserManagementPage::showUserDetails(int recordIndex)
{
    if (recordIndex < 0 || recordIndex >= records_.size()) {
        return;
    }
    selectedRecordIndex_ = recordIndex;
    const UserRecord& record = records_.at(recordIndex);
    avatarLabel_->setText(record.nickname.left(1));
    detailNameLabel_->setText(record.nickname + tr("   ·   ") + record.status);
    detailIdLabel_->setText(tr("用户ID：%1").arg(record.id));
    detailPhoneLabel_->setText(tr("手机号：%1").arg(record.phone));
    detailAccountLabel_->setText(
        tr("实名认证　已实名\n累计消费　¥ 2,586.80\n账户余额　¥ %1\n最近登录　2025-06-01 09:58:12\n常用电站　未来科技城充电站、滨江时代广场充电站")
            .arg(QString::number(record.balance, 'f', 2)));
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
    riskButton_->setEnabled(hasSelection);
    if (!hasSelection) {
        freezeButton_->setText(tr("冻结用户"));
        riskButton_->setText(tr("加入风控关注"));
        return;
    }
    const UserRecord& record = records_.at(selectedRecordIndex_);
    freezeButton_->setText(record.status == tr("冻结") ? tr("解冻用户") : tr("冻结用户"));
    riskButton_->setText(record.isRiskFocused ? tr("移出风控关注") : tr("加入风控关注"));
}

void UserManagementPage::toggleSelectedUserStatus()
{
    if (selectedRecordIndex_ < 0) {
        return;
    }
    UserRecord& record = records_[selectedRecordIndex_];
    const bool isFrozen = record.status == tr("冻结");
    const QString action = isFrozen ? tr("解冻") : tr("冻结");
    const auto choice = QMessageBox::question(
        this, tr("确认%1").arg(action),
        tr("确认要%1用户 %2（手机号后四位 %3）吗？该操作仅更新本地 Mock 状态。")
            .arg(action, record.nickname, record.phone.right(4)));
    if (choice != QMessageBox::Yes) {
        return;
    }
    record.status = isFrozen ? tr("正常") : tr("冻结");
    const int recordIndex = selectedRecordIndex_;
    applyFilters();
    showUserDetails(recordIndex);
    setFeedback(tr("已%1 %2（仅本地 Mock）").arg(action, record.nickname));
}

void UserManagementPage::toggleSelectedRiskFocus()
{
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
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void UserManagementPage::showNextPage()
{
    const int pageCount = (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize;
    if (currentPage_ + 1 >= pageCount) {
        return;
    }
    ++currentPage_;
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void UserManagementPage::setFeedback(const QString& text, bool isError)
{
    feedbackLabel_->setText(text);
    feedbackLabel_->setStyleSheet(isError ? QStringLiteral("color:#ee5757; font-size:13px; font-weight:600;")
                                           : QStringLiteral("color:#6f7d92; font-size:13px;"));
}

} // namespace charging::server
