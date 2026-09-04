#include "charger_management_page.h"

#include "dashboard_visual_widgets.h"
#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPair>
#include <QPen>
#include <QPushButton>
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
    label->setFixedSize(status.size() >= 3 ? 54 : 46, 26);
    return createManagementTableCell(label, parent);
}

class ChargerStatusDonut final : public QWidget
{
public:
    explicit ChargerStatusDonut(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(150, 150);
        setAccessibleName(QObject::tr("电桩状态分布图"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF ringRect = rect().adjusted(21, 21, -21, -21);
        const QList<QPair<QColor, int>> segments = {
            {QColor("#35c6ba"), 298}, {QColor("#8dbbff"), 42},
            {QColor("#ffaf38"), 24}, {QColor("#7e8ba0"), 21},
            {QColor("#ff5a5f"), 15},
        };
        int startAngle = 90 * 16;
        for (const auto& segment : segments) {
            const int spanAngle = -qRound(segment.second * 360.0 / 400.0 * 16.0);
            painter.setPen(QPen(segment.first, 18, Qt::SolidLine, Qt::FlatCap));
            painter.drawArc(ringRect, startAngle, spanAngle);
            startAngle += spanAngle;
        }
        painter.setPen(QColor("#1d2c46"));
        QFont titleFont = painter.font();
        titleFont.setPixelSize(20);
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.drawText(rect().adjusted(0, 50, 0, -34), Qt::AlignCenter, QStringLiteral("1,522"));
        QFont hintFont = painter.font();
        hintFont.setPixelSize(12);
        hintFont.setBold(false);
        painter.setFont(hintFont);
        painter.setPen(QColor("#77849a"));
        painter.drawText(rect().adjusted(0, 76, 0, -10), Qt::AlignCenter, QObject::tr("电桩总数"));
    }
};

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
    setMinimumWidth(1120);
    setMinimumHeight(770);

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
                                tr("奥体中心充电站"), tr("萧山机场充电站")});
    statusComboBox_ = new QComboBox(toolbar);
    statusComboBox_->addItems({tr("状态"), tr("可用"), tr("已预约"), tr("充电中"),
                               tr("故障"), tr("离线")});
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
    feedbackLabel_ = createTextLabel(tr("显示全部 1,522 台电桩"),
                                     QStringLiteral("color:#6f7d92; font-size:13px;"), toolbar);
    feedbackLabel_->setMinimumWidth(142);

    toolbarLayout->addWidget(keywordLineEdit_, 1);
    toolbarLayout->addWidget(stationComboBox_);
    toolbarLayout->addWidget(statusComboBox_);
    toolbarLayout->addWidget(typeComboBox_);
    toolbarLayout->addWidget(powerComboBox_);
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(queryButton);
    toolbarLayout->addWidget(feedbackLabel_);
    layout->addWidget(toolbar);

    auto* bodyLayout = new QHBoxLayout();
    bodyLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(640);
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(18, 18, 18, 16);
    tableLayout->setSpacing(12);
    tableTitleLabel_ = createTextLabel(tr("电桩列表（共 1,522 台）"),
                                       QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"),
                                       tableCard);
    tableLayout->addWidget(tableTitleLabel_);

    tableWidget_ = new QTableWidget(tableCard);
    tableWidget_->setColumnCount(10);
    tableWidget_->setHorizontalHeaderLabels(
        {tr("电桩编号"), tr("所属电站"), tr("类型"), tr("功率"), tr("状态"),
         tr("今日次数"), tr("累计次数"), tr("累计时长"), tr("最后心跳"), tr("操作")});
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
    tableWidget_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Fixed);
    tableWidget_->setColumnWidth(4, 64);
    tableWidget_->setColumnWidth(9, 84);
    tableLayout->addWidget(tableWidget_, 1);

    emptyStateLabel_ = createTextLabel(
        tr("当前筛选条件下没有电桩。请调整条件或点击“重置”。"),
        QStringLiteral("color:#6f7d92; min-height:92px; font-size:14px;"), tableCard);
    emptyStateLabel_->setAlignment(Qt::AlignCenter);
    tableLayout->addWidget(emptyStateLabel_);

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

    auto* insightColumn = new QVBoxLayout();
    insightColumn->setSpacing(16);
    auto* distributionCard = createCompactCard(this);
    distributionCard->setMinimumWidth(244);
    auto* distributionLayout = new QVBoxLayout(distributionCard);
    distributionLayout->setContentsMargins(18, 18, 18, 16);
    distributionLayout->setSpacing(6);
    distributionLayout->addWidget(createTextLabel(tr("状态分布"),
                                                  QStringLiteral("color:#1d2c46; font-size:17px; font-weight:700;"),
                                                  distributionCard));
    distributionLayout->addWidget(new ChargerStatusDonut(distributionCard), 0, Qt::AlignHCenter);
    const QList<QPair<QString, QString>> statusSummary = {
        {tr("在线"), tr("1,256  (82.46%)")}, {tr("空闲"), tr("178  (11.70%)")},
        {tr("充电中"), tr("98  (6.43%)")}, {tr("离线"), tr("88  (5.78%)")},
        {tr("故障"), tr("88  (5.78%)")},
    };
    const QList<QColor> summaryColors = {QColor("#35c6ba"), QColor("#8dbbff"),
                                         QColor("#ffaf38"), QColor("#7e8ba0"), QColor("#ff5a5f")};
    for (int index = 0; index < statusSummary.size(); ++index) {
        auto* rowWidget = new QWidget(distributionCard);
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* dot = new QLabel(rowWidget);
        dot->setFixedSize(7, 7);
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;")
                               .arg(summaryColors.at(index).name()));
        rowLayout->addWidget(dot);
        rowLayout->addWidget(createTextLabel(statusSummary.at(index).first,
                                             QStringLiteral("color:#53627b; font-size:12px;"), rowWidget));
        rowLayout->addStretch();
        rowLayout->addWidget(createTextLabel(statusSummary.at(index).second,
                                             QStringLiteral("color:#53627b; font-size:12px;"), rowWidget));
        distributionLayout->addWidget(rowWidget);
    }
    insightColumn->addWidget(distributionCard);

    auto* alertCard = createCompactCard(this);
    auto* alertLayout = new QVBoxLayout(alertCard);
    alertLayout->setContentsMargins(18, 18, 18, 16);
    alertLayout->setSpacing(8);
    auto* alertHeading = new QHBoxLayout();
    alertHeading->addWidget(createTextLabel(tr("实时告警"),
                                            QStringLiteral("color:#1d2c46; font-size:17px; font-weight:700;"),
                                            alertCard));
    alertCountLabel_ = createTextLabel(tr("5"),
                                       QStringLiteral("background:#ff4d4f; color:white; border-radius:9px;"
                                                      " min-width:18px; min-height:18px; font-size:12px;"
                                                      " font-weight:700;"), alertCard);
    alertCountLabel_->setAlignment(Qt::AlignCenter);
    alertHeading->addWidget(alertCountLabel_);
    alertHeading->addStretch();
    alertLayout->addLayout(alertHeading);
    const QList<QPair<QString, QString>> alerts = {
        {tr("CP10010533"), tr("设备故障")}, {tr("CP10010267"), tr("通讯异常")},
        {tr("CP10010218"), tr("设备离线")}, {tr("CP10010123"), tr("充电中断")},
    };
    for (const auto& alert : alerts) {
        auto* alertButton = new QPushButton(alert.first + QStringLiteral("  ·  ") + alert.second, alertCard);
        alertButton->setCursor(Qt::PointingHandCursor);
        alertButton->setFlat(true);
        alertButton->setStyleSheet(QStringLiteral("QPushButton { color:#53627b; border:0; padding:7px 0;"
                                                   " text-align:left; font-size:13px; }"
                                                   "QPushButton:hover { color:#2878f0; }"));
        alertButton->setAccessibleName(tr("查看告警：%1 %2").arg(alert.first, alert.second));
        connect(alertButton, &QPushButton::clicked, this, [this, code = alert.first]() {
            for (int index = 0; index < records_.size(); ++index) {
                if (records_.at(index).code == code) {
                    showChargerDetails(index);
                    setFeedback(tr("已定位到 %1 的本地 Mock 告警详情").arg(code));
                    return;
                }
            }
            setFeedback(tr("%1 的告警记录仅在本地 Mock 中展示").arg(code));
        });
        alertLayout->addWidget(alertButton);
    }
    alertLayout->addStretch();
    insightColumn->addWidget(alertCard, 1);
    bodyLayout->addLayout(insightColumn);

    auto* detailCard = createManagementDetailCard(tr("电桩详情"), this);
    detailCard->setMinimumWidth(294);
    detailCard->setMaximumWidth(318);
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
    detailLayout->addWidget(restartButton_);
    detailLayout->addWidget(refreshStatusButton_);
    detailLayout->addWidget(clearAlertButton_);
    bodyLayout->addWidget(detailCard);
    layout->addLayout(bodyLayout, 1);

    connect(queryButton, &QPushButton::clicked, this, &ChargerManagementPage::applyFilters);
    connect(resetButton, &QPushButton::clicked, this, &ChargerManagementPage::resetFilters);
    connect(keywordLineEdit_, &QLineEdit::returnPressed, this, &ChargerManagementPage::applyFilters);
    connect(previousPageButton_, &QPushButton::clicked, this, &ChargerManagementPage::showPreviousPage);
    connect(nextPageButton_, &QPushButton::clicked, this, &ChargerManagementPage::showNextPage);
    connect(restartButton_, &QPushButton::clicked, this, &ChargerManagementPage::restartSelectedCharger);
    connect(refreshStatusButton_, &QPushButton::clicked, this,
            &ChargerManagementPage::refreshSelectedStatus);
    connect(clearAlertButton_, &QPushButton::clicked, this,
            &ChargerManagementPage::clearSelectedAlert);
    connect(tableWidget_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr) {
            showChargerDetails(item->data(Qt::UserRole).toInt());
        }
    });

    createMockRecords();
    applyFilters();
}

void ChargerManagementPage::createMockRecords()
{
    records_ = {
        {tr("CP10010086"), tr("未来科技城充电站"), tr("直流桩"), tr("120kW"), tr("充电中"), 24, 2312,
         tr("356h 22m"), tr("2025-06-01 10:28:45")},
        {tr("CP10010123"), tr("滨江智慧园充电站"), tr("直流桩"), tr("60kW"), tr("可用"), 12, 1028,
         tr("180h 14m"), tr("2025-06-01 10:28:32")},
        {tr("CP10010205"), tr("城西银泰充电站"), tr("交流桩"), tr("7kW"), tr("可用"), 8, 648,
         tr("96h 05m"), tr("2025-06-01 10:28:19")},
        {tr("CP10010218"), tr("奥体中心充电站"), tr("直流桩"), tr("120kW"), tr("离线"), 0, 532,
         tr("88h 47m"), tr("2025-06-01 09:42:10")},
        {tr("CP10010267"), tr("萧山机场充电站"), tr("直流桩"), tr("180kW"), tr("故障"), 0, 212,
         tr("32h 06m"), tr("2025-06-01 08:11:09")},
        {tr("CP10010345"), tr("城西银泰充电站"), tr("交流桩"), tr("7kW"), tr("可用"), 6, 421,
         tr("62h 13m"), tr("2025-06-01 10:28:01")},
        {tr("CP10010378"), tr("未来科技城充电站"), tr("直流桩"), tr("120kW"), tr("充电中"), 18, 1857,
         tr("284h 36m"), tr("2025-06-01 10:27:55")},
        {tr("CP10010402"), tr("奥体中心充电站"), tr("交流桩"), tr("7kW"), tr("已预约"), 5, 309,
         tr("45h 21m"), tr("2025-06-01 10:27:41")},
        {tr("CP10010495"), tr("滨江智慧园充电站"), tr("直流桩"), tr("60kW"), tr("可用"), 9, 712,
         tr("103h 50m"), tr("2025-06-01 10:27:28")},
        {tr("CP10010533"), tr("富阳智造港充电站"), tr("交流桩"), tr("7kW"), tr("离线"), 0, 176,
         tr("24h 18m"), tr("2025-06-01 09:31:17")},
        {tr("CP10010614"), tr("未来科技城充电站"), tr("直流桩"), tr("120kW"), tr("可用"), 11, 855,
         tr("123h 04m"), tr("2025-06-01 10:26:42")},
        {tr("CP10010628"), tr("萧山机场充电站"), tr("直流桩"), tr("180kW"), tr("可用"), 14, 1124,
         tr("169h 32m"), tr("2025-06-01 10:26:08")},
    };
}

bool ChargerManagementPage::recordMatchesFilters(const ChargerRecord& record) const
{
    const QString keyword = keywordLineEdit_->text().trimmed();
    const bool matchesKeyword = keyword.isEmpty() || record.code.contains(keyword, Qt::CaseInsensitive);
    const bool matchesStation = stationComboBox_->currentIndex() == 0
        || record.station == stationComboBox_->currentText();
    const bool matchesStatus = statusComboBox_->currentIndex() == 0
        || record.status == statusComboBox_->currentText();
    const bool matchesType = typeComboBox_->currentIndex() == 0
        || record.type == typeComboBox_->currentText();
    const bool matchesPower = powerComboBox_->currentIndex() == 0
        || record.power == powerComboBox_->currentText();
    return matchesKeyword && matchesStation && matchesStatus && matchesType && matchesPower;
}

void ChargerManagementPage::applyFilters()
{
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
    setFeedback(tr("已重置筛选条件，显示全部本地 Mock 电桩"));
}

void ChargerManagementPage::rebuildTable()
{
    const int pageCount = qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = currentPage_ * kPageSize;
    const int end = qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const ChargerRecord& record = records_.at(recordIndex);
        const QList<QString> values = {record.code, record.station, record.type, record.power, QString(),
                                       QString::number(record.todaySessions),
                                       QString::number(record.totalSessions), record.totalDuration,
                                       record.lastHeartbeat, QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 4 || column == 9) {
                continue;
            }
            auto* item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, recordIndex);
            item->setTextAlignment(Qt::AlignCenter);
            tableWidget_->setItem(row, column, item);
        }
        tableWidget_->setCellWidget(row, 4, createCompactStatusTag(record.status, tableWidget_));
        auto* detailButton = new QPushButton(tr("详情"), tableWidget_);
        detailButton->setObjectName(QStringLiteral("tableActionButton"));
        detailButton->setAccessibleName(tr("查看 %1 的详情").arg(record.code));
        connect(detailButton, &QPushButton::clicked, this, [this, recordIndex]() {
            showChargerDetails(recordIndex);
        });
        tableWidget_->setCellWidget(row, 9, createManagementTableCell(detailButton, tableWidget_));
    }
    tableTitleLabel_->setText(tr("电桩列表（共 %1 台）").arg(filteredRecordIndexes_.size()));
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
    emptyStateLabel_->setVisible(isEmpty);
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
    detailCodeLabel_->setText(record.code);
    detailStatusLabel_->setText(record.status);
    detailStatusLabel_->setStyleSheet(statusStyle(record.status));
    detailStationLabel_->setText(record.station + tr("\n更新时间：") + record.lastHeartbeat);
    detailBasicInfoLabel_->setText(
        tr("电桩类型　%1\n额定功率　%2\n额定电压　750V\n设备版本　V2.3.6\n出厂编号　202408060086")
            .arg(record.type, record.power));
    detailRuntimeInfoLabel_->setText(
        tr("今日次数　%1 次\n累计次数　%2 次\n累计时长　%3\n当前功率　%4\n最后心跳　%5")
            .arg(QString::number(record.todaySessions), QString::number(record.totalSessions),
                 record.totalDuration, record.status == tr("充电中") ? tr("68.4 kW") : tr("--"),
                 record.lastHeartbeat));
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
    restartButton_->setToolTip(canRestart ? tr("仅更新本地 Mock 状态")
                                          : tr("充电中或已预约的电桩不可远程重启"));
    refreshStatusButton_->setEnabled(hasSelection);
    const bool canClearAlert = hasSelection && (status == tr("故障") || status == tr("离线"));
    clearAlertButton_->setEnabled(canClearAlert);
    clearAlertButton_->setToolTip(canClearAlert ? tr("仅解除本地 Mock 告警")
                                                : tr("当前电桩没有可解除的告警"));
}

void ChargerManagementPage::refreshSelectedStatus()
{
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
        tr("确认对电桩 %1 执行远程重启吗？该操作仅更新本地 Mock 状态，不会发送 TCP 命令或写入数据库。")
            .arg(record.code));
    if (choice != QMessageBox::Yes) {
        return;
    }
    record.status = tr("可用");
    record.lastHeartbeat = tr("2025-06-01 10:30:00");
    rebuildTable();
    showChargerDetails(selectedRecordIndex_);
    setFeedback(tr("已完成 %1 的本地 Mock 远程重启").arg(record.code));
}

void ChargerManagementPage::clearSelectedAlert()
{
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
    rebuildTable();
    showChargerDetails(selectedRecordIndex_);
    alertCountLabel_->setText(QString::number(qMax(0, alertCountLabel_->text().toInt() - 1)));
    setFeedback(tr("已解除 %1 的本地 Mock 告警").arg(record.code));
}

void ChargerManagementPage::showPreviousPage()
{
    if (currentPage_ <= 0) {
        return;
    }
    --currentPage_;
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void ChargerManagementPage::showNextPage()
{
    const int pageCount = (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize;
    if (currentPage_ + 1 >= pageCount) {
        return;
    }
    ++currentPage_;
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void ChargerManagementPage::setFeedback(const QString& text)
{
    feedbackLabel_->setText(text);
}

} // namespace charging::server
