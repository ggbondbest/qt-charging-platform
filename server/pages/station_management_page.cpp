#include "station_management_page.h"

#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLinearGradient>
#include <QList>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QPen>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QtMath>

namespace charging::server {

namespace {

constexpr int kPageSize = 8;

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
    label->setFixedSize(status.size() >= 3 ? 54 : 46, 26);
    return createManagementTableCell(label, parent);
}

QFrame* createCompactCard(QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("contentCard"));
    return card;
}

class StationPreviewWidget final : public QWidget
{
public:
    explicit StationPreviewWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(132);
        setAccessibleName(QObject::tr("电站示意图"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF canvas = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        QLinearGradient background(canvas.topLeft(), canvas.bottomRight());
        background.setColorAt(0.0, QColor("#cce6ff"));
        background.setColorAt(0.52, QColor("#eff7ff"));
        background.setColorAt(0.53, QColor("#dfe9ef"));
        background.setColorAt(1.0, QColor("#bdcbd4"));
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(canvas, 10, 10);
        painter.setBrush(QColor("#6d8e67"));
        painter.drawEllipse(QRectF(canvas.left() + 13, canvas.top() + 36, 34, 41));
        painter.drawEllipse(QRectF(canvas.right() - 50, canvas.top() + 29, 37, 48));
        painter.setBrush(QColor("#ffffff"));
        painter.drawRoundedRect(QRectF(canvas.left() + 53, canvas.top() + 48, canvas.width() - 105, 38), 3, 3);
        painter.setBrush(QColor("#335c7b"));
        QPainterPath roof;
        roof.moveTo(canvas.left() + 41, canvas.top() + 47);
        roof.lineTo(canvas.center().x(), canvas.top() + 22);
        roof.lineTo(canvas.right() - 37, canvas.top() + 47);
        roof.closeSubpath();
        painter.drawPath(roof);
        painter.setBrush(QColor("#2878f0"));
        painter.drawRoundedRect(QRectF(canvas.left() + 65, canvas.top() + 57, 18, 28), 3, 3);
        painter.drawRoundedRect(QRectF(canvas.right() - 83, canvas.top() + 57, 18, 28), 3, 3);
        painter.setBrush(QColor("#42566d"));
        painter.drawRoundedRect(QRectF(canvas.left() + 88, canvas.bottom() - 37, 55, 20), 8, 8);
        painter.setBrush(QColor("#dce9f4"));
        painter.drawRoundedRect(QRectF(canvas.left() + 97, canvas.bottom() - 33, 26, 7), 3, 3);
    }
};

class UtilizationTrendWidget final : public QWidget
{
public:
    explicit UtilizationTrendWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(118);
        setAccessibleName(QObject::tr("今日利用率趋势图"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF chart = rect().adjusted(8, 10, -8, -22);
        painter.setPen(QPen(QColor("#e9eef5"), 1));
        for (int row = 0; row < 3; ++row) {
            const qreal y = chart.top() + chart.height() * row / 2.0;
            painter.drawLine(QPointF(chart.left(), y), QPointF(chart.right(), y));
        }
        const QList<qreal> values = {0.32, 0.46, 0.46, 0.60, 0.63, 0.60,
                                     0.72, 0.66, 0.70, 0.78, 0.72, 0.75,
                                     0.80, 0.77, 0.72, 0.75, 0.71};
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
        fill.setColorAt(0.0, QColor(40, 120, 240, 55));
        fill.setColorAt(1.0, QColor(40, 120, 240, 3));
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(area);
        painter.setPen(QPen(QColor("#2878f0"), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(line);
        painter.setPen(QColor("#8795a9"));
        QFont font = painter.font();
        font.setPixelSize(11);
        painter.setFont(font);
        painter.drawText(QRectF(chart.left(), chart.bottom() + 5, 35, 13), Qt::AlignLeft,
                         QObject::tr("00:00"));
        painter.drawText(QRectF(chart.center().x() - 18, chart.bottom() + 5, 36, 13),
                         Qt::AlignCenter, QObject::tr("12:00"));
        painter.drawText(QRectF(chart.right() - 35, chart.bottom() + 5, 35, 13), Qt::AlignRight,
                         QObject::tr("24:00"));
    }
};

} // namespace

StationManagementPage::StationManagementPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("stationManagementPage"));
    setMinimumWidth(1120);
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
    feedbackLabel_->setMinimumWidth(138);
    toolbarLayout->addWidget(keywordLineEdit_, 1);
    toolbarLayout->addWidget(cityComboBox_);
    toolbarLayout->addWidget(districtComboBox_);
    toolbarLayout->addWidget(statusComboBox_);
    toolbarLayout->addWidget(resetButton);
    toolbarLayout->addWidget(queryButton);
    toolbarLayout->addWidget(addButton);
    toolbarLayout->addWidget(feedbackLabel_);
    layout->addWidget(toolbar);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(720);
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
    tableWidget_->setColumnWidth(8, 64);
    tableWidget_->setColumnWidth(9, 132);
    tableLayout->addWidget(tableWidget_, 1);
    emptyStateLabel_ = createTextLabel(tr("当前筛选条件下没有电站。请调整条件或点击“重置”。"),
                                       QStringLiteral("color:#6f7d92; min-height:92px; font-size:14px;"), tableCard);
    emptyStateLabel_->setAlignment(Qt::AlignCenter);
    tableLayout->addWidget(emptyStateLabel_);
    auto* pagerLayout = new QHBoxLayout();
    pagerLayout->addWidget(createTextLabel(tr("每页 8 条"), QStringLiteral("color:#718098; font-size:13px;"), tableCard));
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
    detailCard->setMinimumWidth(298);
    detailCard->setMaximumWidth(324);
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
    detailLayout->addWidget(new StationPreviewWidget(detailCard));
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
    auto* trendHeading = new QHBoxLayout();
    trendHeading->addWidget(createTextLabel(tr("今日利用率趋势"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    trendHeading->addStretch();
    trendHeading->addWidget(createTextLabel(tr("68.4%"), QStringLiteral("color:#1d2c46; font-size:15px; font-weight:700;"), detailCard));
    detailLayout->addLayout(trendHeading);
    detailLayout->addWidget(new UtilizationTrendWidget(detailCard));
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
        {tr("STN000328"), tr("未来科技城充电站"), tr("杭州市"), tr("余杭区"), tr("文一西路969号未来科技城通勤园停车场B区"), tr("运营中"), 24, 18, 6, 68, 68, tr("张伟"), tr("138 6712 8888")},
        {tr("STN000329"), tr("西溪湿地公园充电站"), tr("杭州市"), tr("西湖区"), tr("天目山路518号西溪湿地北门"), tr("运营中"), 16, 10, 6, 52, 62, tr("李娜"), tr("139 5800 2146")},
        {tr("STN000330"), tr("滨江星光大道充电站"), tr("杭州市"), tr("滨江区"), tr("江南大道228号星光大道地下车库"), tr("运营中"), 20, 14, 6, 74, 71, tr("王凯"), tr("136 7574 6012")},
        {tr("STN000331"), tr("萧山机场P4充电站"), tr("杭州市"), tr("萧山区"), tr("机场高速萧山机场P4停车场"), tr("运营中"), 30, 24, 6, 96, 75, tr("陈力"), tr("137 3815 3377")},
        {tr("STN000332"), tr("钱江世纪城充电站"), tr("杭州市"), tr("滨江区"), tr("民和路与奔竞大道交叉口"), tr("运营中"), 18, 12, 6, 60, 65, tr("赵敏"), tr("135 8821 9104")},
        {tr("STN000333"), tr("城西银泰充电站"), tr("杭州市"), tr("拱墅区"), tr("丰潭路380号城西银泰城B2层"), tr("空闲"), 12, 8, 4, 28, 53, tr("周辰"), tr("186 6804 2241")},
        {tr("STN000334"), tr("下沙大学城充电站"), tr("杭州市"), tr("余杭区"), tr("学源街998号下沙大学城东区"), tr("运营中"), 22, 14, 8, 46, 49, tr("朱琳"), tr("158 5814 7732")},
        {tr("STN000335"), tr("临平地铁站充电站"), tr("杭州市"), tr("余杭区"), tr("临平地铁站南广场地下停车场"), tr("空闲"), 16, 8, 8, 32, 48, tr("孙洋"), tr("150 6810 1835")},
        {tr("STN000336"), tr("宁波东部新城充电站"), tr("宁波市"), tr("鄞州区"), tr("宁穿路1888号东部新城停车楼"), tr("已停用"), 14, 10, 4, 0, 0, tr("吴峰"), tr("188 5726 1300")},
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
    setFeedback(tr("已重置筛选条件，显示全部本地 Mock 电站"));
}

void StationManagementPage::rebuildTable()
{
    const int pageCount = qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = currentPage_ * kPageSize;
    const int end = qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const StationRecord& record = records_.at(recordIndex);
        const QList<QString> values = {record.name, record.city + tr(" / ") + record.district, record.address,
                                       QString::number(record.chargerCount), QString::number(record.fastChargerCount),
                                       QString::number(record.slowChargerCount), QString::number(record.todayOrders),
                                       QString::number(record.utilizationPercent) + tr("%"), QString(), QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 8 || column == 9) {
                continue;
            }
            auto* item = new QTableWidgetItem(values.at(column));
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
    tableTitleLabel_->setText(tr("电站列表（共 %1 座）").arg(filteredRecordIndexes_.size()));
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
    emptyStateLabel_->setVisible(isEmpty);
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
    detailNameLabel_->setText(record.name);
    detailIdLabel_->setText(tr("电站ID：%1").arg(record.code));
    detailStatusLabel_->setText(record.status);
    detailStatusLabel_->setStyleSheet(stationStatusStyle(record.status));
    detailAddressLabel_->setText(record.city + record.district + tr("\n") + record.address);
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
    toggleStatusButton_->setToolTip(tr("仅更新本地 Mock 状态"));
}

void StationManagementPage::showAddStationDialog()
{
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
    dialog.setWindowTitle(isEditing ? tr("编辑电站（Mock）") : tr("新增电站（Mock）"));
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
        contactLineEdit->setText(record.contactName);
        phoneLineEdit->setText(record.contactPhone);
    } else {
        codeLineEdit->setPlaceholderText(tr("例如 STN000337"));
        nameLineEdit->setPlaceholderText(tr("例如 西湖文体中心充电站"));
    }
    formLayout->addRow(tr("电站编号 *"), codeLineEdit);
    formLayout->addRow(tr("电站名称 *"), nameLineEdit);
    formLayout->addRow(tr("城市 *"), cityComboBox);
    formLayout->addRow(tr("区域 *"), districtLineEdit);
    formLayout->addRow(tr("详细地址 *"), addressLineEdit);
    formLayout->addRow(tr("负责人 *"), contactLineEdit);
    formLayout->addRow(tr("联系电话 *"), phoneLineEdit);
    layout->addLayout(formLayout);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, Qt::Horizontal, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(isEditing ? tr("保存修改") : tr("确认新增"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, codeLineEdit, nameLineEdit, districtLineEdit,
                                                              addressLineEdit, contactLineEdit, phoneLineEdit]() {
        if (codeLineEdit->text().trimmed().isEmpty() || nameLineEdit->text().trimmed().isEmpty()
            || districtLineEdit->text().trimmed().isEmpty() || addressLineEdit->text().trimmed().isEmpty()
            || contactLineEdit->text().trimmed().isEmpty() || phoneLineEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, QObject::tr("请补全信息"), QObject::tr("所有带 * 的字段均为必填项。"));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (isEditing) {
        StationRecord& record = records_[recordIndex];
        record.name = nameLineEdit->text().trimmed();
        record.city = cityComboBox->currentText();
        record.district = districtLineEdit->text().trimmed();
        record.address = addressLineEdit->text().trimmed();
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
                     districtLineEdit->text().trimmed(), addressLineEdit->text().trimmed(), tr("运营中"),
                     12, 8, 4, 0, 0, contactLineEdit->text().trimmed(), phoneLineEdit->text().trimmed()});
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
        isStopped ? tr("确认恢复电站 %1 的运营吗？该操作仅更新本地 Mock 状态。\n真实操作需等待 Service 接口。").arg(record.name)
                  : tr("确认暂停电站 %1 的运营吗？该操作仅更新本地 Mock 状态。\n真实操作需等待 Service 接口。").arg(record.name));
    if (choice != QMessageBox::Yes) {
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
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void StationManagementPage::showNextPage()
{
    const int pageCount = (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize;
    if (currentPage_ + 1 >= pageCount) {
        return;
    }
    ++currentPage_;
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void StationManagementPage::setFeedback(const QString& text)
{
    feedbackLabel_->setText(text);
}

} // namespace charging::server
