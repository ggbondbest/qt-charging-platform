#include "order_management_page.h"

#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFrame>
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

QString orderStatusStyle(const QString& status)
{
    if (status == QObject::tr("进行中")) {
        return QStringLiteral("background:#fff3df; color:#f08a1c; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("已完成")) {
        return QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("已支付")) {
        return QStringLiteral("background:#eaf2ff; color:#337df1; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == QObject::tr("异常")) {
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
    label->setStyleSheet(orderStatusStyle(status));
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

class PaymentDonutWidget final : public QWidget
{
public:
    explicit PaymentDonutWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(138, 138);
        setAccessibleName(QObject::tr("今日支付状态分布图"));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF ring = rect().adjusted(22, 22, -22, -22);
        const QList<QPair<QColor, int>> segments = {
            {QColor("#35c6ba"), 329}, {QColor("#ffad39"), 31},
            {QColor("#8ebcff"), 24}, {QColor("#ed6365"), 16},
        };
        int startAngle = 90 * 16;
        for (const auto& segment : segments) {
            const int spanAngle = -qRound(segment.second * 360.0 / 400.0 * 16.0);
            painter.setPen(QPen(segment.first, 17, Qt::SolidLine, Qt::FlatCap));
            painter.drawArc(ring, startAngle, spanAngle);
            startAngle += spanAngle;
        }
        painter.setPen(QColor("#1d2c46"));
        QFont font = painter.font();
        font.setPixelSize(19);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(rect().adjusted(0, 48, 0, -30), Qt::AlignCenter, QStringLiteral("1,248"));
        font.setPixelSize(11);
        font.setBold(false);
        painter.setFont(font);
        painter.setPen(QColor("#77849a"));
        painter.drawText(rect().adjusted(0, 72, 0, -9), Qt::AlignCenter, QObject::tr("今日订单"));
    }
};

} // namespace

OrderManagementPage::OrderManagementPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("orderManagementPage"));
    setMinimumWidth(1180);
    setMinimumHeight(760);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(18);

    auto* metricsLayout = new QHBoxLayout();
    metricsLayout->setSpacing(16);
    metricsLayout->addWidget(createManagementMetricCard(
        tr("今日订单"), tr("1,248"), tr(" 笔"), tr("较昨日  +156 (+14.28%)  ↑"), QColor("#347cf6"), 1, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("今日营收"), tr("¥ 18,742.68"), QString(), tr("较昨日  +2,356.32 (+14.36%)  ↑"), QColor("#43c7bc"), 0, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("充电中订单"), tr("286"), tr(" 笔"), tr("较昨日  -18 (-5.93%)  ↓"), QColor("#ff9a26"), 2, this));
    metricsLayout->addWidget(createManagementMetricCard(
        tr("异常订单"), tr("16"), tr(" 笔"), tr("较昨日  -6 (-27.27%)  ↓"), QColor("#ff5b61"), 3, this));
    layout->addLayout(metricsLayout);

    auto* toolbar = createCompactCard(this);
    auto* toolbarLayout = new QVBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(18, 12, 18, 12);
    toolbarLayout->setSpacing(9);
    auto* firstLine = new QHBoxLayout();
    firstLine->setSpacing(10);
    orderNumberLineEdit_ = new QLineEdit(toolbar);
    orderNumberLineEdit_->setPlaceholderText(tr("请输入订单号"));
    orderNumberLineEdit_->setMinimumWidth(142);
    userLineEdit_ = new QLineEdit(toolbar);
    userLineEdit_->setPlaceholderText(tr("请输入用户名"));
    userLineEdit_->setMinimumWidth(142);
    phoneLineEdit_ = new QLineEdit(toolbar);
    phoneLineEdit_->setPlaceholderText(tr("请输入手机号"));
    phoneLineEdit_->setMinimumWidth(142);
    stationComboBox_ = new QComboBox(toolbar);
    stationComboBox_->addItems({tr("全部电站"), tr("未来科技城充电站"), tr("滨江智慧园充电站"),
                                tr("城西西溪充电站"), tr("奥体中心充电站")});
    chargerComboBox_ = new QComboBox(toolbar);
    chargerComboBox_->addItems({tr("全部电桩"), tr("CP10010086"), tr("CP10010123"), tr("CP10010205"),
                                tr("CP10010218"), tr("CP10010267")});
    for (auto* comboBox : {stationComboBox_, chargerComboBox_}) {
        comboBox->setMinimumWidth(132);
        configureManagementComboBox(comboBox);
    }
    firstLine->addWidget(createTextLabel(tr("订单号"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    firstLine->addWidget(orderNumberLineEdit_);
    firstLine->addWidget(createTextLabel(tr("用户"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    firstLine->addWidget(userLineEdit_);
    firstLine->addWidget(createTextLabel(tr("手机号"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    firstLine->addWidget(phoneLineEdit_);
    firstLine->addWidget(createTextLabel(tr("电站"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    firstLine->addWidget(stationComboBox_);
    firstLine->addWidget(createTextLabel(tr("电桩"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    firstLine->addWidget(chargerComboBox_);
    toolbarLayout->addLayout(firstLine);
    auto* secondLine = new QHBoxLayout();
    secondLine->setSpacing(10);
    statusComboBox_ = new QComboBox(toolbar);
    statusComboBox_->addItems({tr("全部状态"), tr("进行中"), tr("已完成"), tr("已支付"), tr("异常"), tr("已退款")});
    dateRangeComboBox_ = new QComboBox(toolbar);
    dateRangeComboBox_->addItems({tr("全部时间"), tr("今日"), tr("近 7 日")});
    for (auto* comboBox : {statusComboBox_, dateRangeComboBox_}) {
        comboBox->setMinimumWidth(126);
        configureManagementComboBox(comboBox);
    }
    auto* resetButton = new QPushButton(tr("重置"), toolbar);
    resetButton->setObjectName(QStringLiteral("secondaryButton"));
    auto* queryButton = new QPushButton(tr("查询"), toolbar);
    queryButton->setObjectName(QStringLiteral("primaryButton"));
    feedbackLabel_ = createTextLabel(tr("显示全部 12,845 笔订单"), QStringLiteral("color:#6f7d92; font-size:13px;"), toolbar);
    secondLine->addWidget(createTextLabel(tr("订单状态"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    secondLine->addWidget(statusComboBox_);
    secondLine->addWidget(createTextLabel(tr("时间范围"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    secondLine->addWidget(dateRangeComboBox_);
    secondLine->addStretch();
    secondLine->addWidget(resetButton);
    secondLine->addWidget(queryButton);
    secondLine->addWidget(feedbackLabel_);
    toolbarLayout->addLayout(secondLine);
    layout->addWidget(toolbar);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(830);
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(18, 18, 18, 16);
    tableLayout->setSpacing(12);
    tableTitleLabel_ = createTextLabel(tr("订单列表（共 12,845 笔）"),
                                       QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"), tableCard);
    tableLayout->addWidget(tableTitleLabel_);
    tableWidget_ = new QTableWidget(tableCard);
    tableWidget_->setColumnCount(12);
    tableWidget_->setHorizontalHeaderLabels(
        {tr("订单号"), tr("用户"), tr("电站"), tr("电桩"), tr("开始时间"), tr("充电时长"),
         tr("充电量 (kWh)"), tr("充电费"), tr("服务费"), tr("总金额"), tr("状态"), tr("操作")});
    tableWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget_->setFocusPolicy(Qt::NoFocus);
    tableWidget_->setAlternatingRowColors(true);
    tableWidget_->setShowGrid(false);
    tableWidget_->verticalHeader()->setVisible(false);
    tableWidget_->verticalHeader()->setDefaultSectionSize(48);
    tableWidget_->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    tableWidget_->horizontalHeader()->setStretchLastSection(false);
    tableWidget_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(10, QHeaderView::Fixed);
    tableWidget_->horizontalHeader()->setSectionResizeMode(11, QHeaderView::Fixed);
    tableWidget_->setColumnWidth(10, 64);
    tableWidget_->setColumnWidth(11, 72);
    tableLayout->addWidget(tableWidget_, 1);
    emptyStateLabel_ = createTextLabel(tr("当前筛选条件下没有订单。请调整条件或点击“重置”。"),
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

    auto* detailCard = createManagementDetailCard(tr("订单详情"), this);
    detailCard->setMinimumWidth(294);
    detailCard->setMaximumWidth(322);
    auto* detailLayout = qobject_cast<QVBoxLayout*>(detailCard->layout());
    auto* titleRow = new QHBoxLayout();
    detailOrderNumberLabel_ = createTextLabel(QString(), QStringLiteral("color:#273751; font-size:14px; font-weight:700;"), detailCard);
    detailStatusLabel_ = createStatusTag(tr("进行中"), detailCard);
    titleRow->addWidget(detailOrderNumberLabel_);
    titleRow->addStretch();
    titleRow->addWidget(detailStatusLabel_);
    detailLayout->addLayout(titleRow);
    detailCreatedAtLabel_ = createTextLabel(QString(), QStringLiteral("color:#718098; font-size:13px;"), detailCard);
    detailLayout->addWidget(detailCreatedAtLabel_);
    auto* divider = new QFrame(detailCard);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color:#edf1f7;"));
    detailLayout->addWidget(divider);
    detailLayout->addWidget(createTextLabel(tr("充电信息"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    chargingInfoLabel_ = createTextLabel(QString(), QStringLiteral("color:#55647c; font-size:13px;"), detailCard);
    chargingInfoLabel_->setWordWrap(true);
    detailLayout->addWidget(chargingInfoLabel_);
    detailLayout->addWidget(createTextLabel(tr("费用明细"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    feeInfoLabel_ = createTextLabel(QString(), QStringLiteral("color:#55647c; font-size:13px;"), detailCard);
    feeInfoLabel_->setWordWrap(true);
    detailLayout->addWidget(feeInfoLabel_);
    paymentInfoLabel_ = createTextLabel(QString(), QStringLiteral("color:#55647c; font-size:13px;"), detailCard);
    paymentInfoLabel_->setWordWrap(true);
    detailLayout->addWidget(paymentInfoLabel_);
    auto* paymentHeading = new QHBoxLayout();
    paymentHeading->addWidget(createTextLabel(tr("支付状态分布（今日）"), QStringLiteral("color:#34435b; font-size:14px; font-weight:700;"), detailCard));
    paymentHeading->addStretch();
    detailLayout->addLayout(paymentHeading);
    auto* paymentRow = new QHBoxLayout();
    paymentRow->addWidget(new PaymentDonutWidget(detailCard));
    auto* legend = new QVBoxLayout();
    const QList<QPair<QString, QColor>> payments = {{tr("已支付 82.21%"), QColor("#35c6ba")},
                                                     {tr("待支付 7.69%"), QColor("#ffad39")},
                                                     {tr("已退款 6.25%"), QColor("#8ebcff")},
                                                     {tr("支付失败 3.85%"), QColor("#ed6365")}};
    for (const auto& payment : payments) {
        auto* row = new QWidget(detailCard);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* dot = new QLabel(row);
        dot->setFixedSize(7, 7);
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:3px;").arg(payment.second.name()));
        rowLayout->addWidget(dot);
        rowLayout->addWidget(createTextLabel(payment.first, QStringLiteral("color:#65738a; font-size:11px;"), row));
        legend->addWidget(row);
    }
    paymentRow->addLayout(legend, 1);
    detailLayout->addLayout(paymentRow);
    detailLayout->addStretch();
    refreshButton_ = new QPushButton(tr("刷新详情"), detailCard);
    refreshButton_->setObjectName(QStringLiteral("secondaryButton"));
    refreshButton_->setAccessibleName(tr("刷新当前订单的本地 Mock 详情"));
    detailLayout->addWidget(refreshButton_);
    contentLayout->addWidget(detailCard);
    layout->addLayout(contentLayout, 1);

    connect(queryButton, &QPushButton::clicked, this, &OrderManagementPage::applyFilters);
    connect(resetButton, &QPushButton::clicked, this, &OrderManagementPage::resetFilters);
    connect(orderNumberLineEdit_, &QLineEdit::returnPressed, this, &OrderManagementPage::applyFilters);
    connect(userLineEdit_, &QLineEdit::returnPressed, this, &OrderManagementPage::applyFilters);
    connect(phoneLineEdit_, &QLineEdit::returnPressed, this, &OrderManagementPage::applyFilters);
    connect(previousPageButton_, &QPushButton::clicked, this, &OrderManagementPage::showPreviousPage);
    connect(nextPageButton_, &QPushButton::clicked, this, &OrderManagementPage::showNextPage);
    connect(refreshButton_, &QPushButton::clicked, this, &OrderManagementPage::refreshSelectedOrder);
    connect(tableWidget_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr) {
            showOrderDetails(item->data(Qt::UserRole).toInt());
        }
    });
    createMockRecords();
    applyFilters();
}

void OrderManagementPage::createMockRecords()
{
    records_ = {
        {tr("CP202506010001"), tr("张先生"), tr("138****5678"), tr("未来科技城充电站"), tr("CP10010086"), tr("直流桩"), tr("进行中"), tr("2025-06-01 10:28:45"), tr("36分22秒"), 24.16, 34.62, 6.92, 0.00, tr("微信支付"), tr("已支付")},
        {tr("CP202506010002"), tr("李女士"), tr("159****8899"), tr("滨江智慧园充电站"), tr("CP10010123"), tr("直流桩"), tr("已完成"), tr("2025-06-01 09:56:13"), tr("1时48分"), 38.24, 54.78, 10.96, 0.00, tr("支付宝"), tr("已支付")},
        {tr("CP202506010003"), tr("王先生"), tr("137****1122"), tr("城西西溪充电站"), tr("CP10010205"), tr("交流桩"), tr("已支付"), tr("2025-06-01 09:31:17"), tr("56分05秒"), 23.58, 31.86, 6.37, 0.00, tr("微信支付"), tr("已支付")},
        {tr("CP202506010004"), tr("陈女士"), tr("186****3344"), tr("奥体中心充电站"), tr("CP10010218"), tr("直流桩"), tr("异常"), tr("2025-06-01 08:47:25"), tr("28分47秒"), 16.72, 22.57, 4.51, 0.00, tr("—"), tr("支付失败")},
        {tr("CP202506010005"), tr("刘先生"), tr("152****7788"), tr("萧山机场充电站"), tr("CP10010267"), tr("直流桩"), tr("已完成"), tr("2025-06-01 07:55:41"), tr("32分06秒"), 18.34, 24.74, 4.95, 0.00, tr("支付宝"), tr("已支付")},
        {tr("CP202506010006"), tr("赵先生"), tr("139****9900"), tr("西溪湿地充电站"), tr("CP10010345"), tr("交流桩"), tr("已完成"), tr("2025-06-01 07:12:30"), tr("1时38分"), 32.61, 44.02, 8.80, 0.00, tr("微信支付"), tr("已支付")},
        {tr("CP202506010007"), tr("吴女士"), tr("158****2211"), tr("社区便民充电站"), tr("CP10010378"), tr("交流桩"), tr("已退款"), tr("2025-06-01 06:41:18"), tr("24分36秒"), 12.48, 16.85, 3.37, 20.22, tr("支付宝"), tr("已退款")},
        {tr("CP202506010008"), tr("孙先生"), tr("187****4455"), tr("下沙大学城充电站"), tr("CP10010402"), tr("交流桩"), tr("已支付"), tr("2025-06-01 06:15:56"), tr("45分12秒"), 20.13, 27.18, 5.44, 0.00, tr("微信支付"), tr("已支付")},
        {tr("CP202506010009"), tr("周女士"), tr("150****6677"), tr("临平新城充电站"), tr("CP10010495"), tr("直流桩"), tr("已完成"), tr("2025-06-01 05:30:22"), tr("1时03分"), 28.33, 38.25, 7.65, 0.00, tr("支付宝"), tr("已支付")},
        {tr("CP202506010010"), tr("黄先生"), tr("188****5566"), tr("富阳商旅充电站"), tr("CP10010533"), tr("交流桩"), tr("异常"), tr("2025-06-01 05:05:11"), tr("24分18秒"), 10.57, 14.28, 2.85, 0.00, tr("—"), tr("待支付")},
        {tr("CP202505310011"), tr("杨女士"), tr("136****3456"), tr("未来科技城充电站"), tr("CP10010086"), tr("直流桩"), tr("已完成"), tr("2025-05-31 23:42:11"), tr("54分36秒"), 26.80, 38.16, 7.63, 0.00, tr("微信支付"), tr("已支付")},
        {tr("CP202505310012"), tr("何先生"), tr("131****8024"), tr("滨江智慧园充电站"), tr("CP10010123"), tr("直流桩"), tr("已支付"), tr("2025-05-31 22:17:40"), tr("48分10秒"), 21.44, 30.02, 6.00, 0.00, tr("支付宝"), tr("已支付")},
    };
}

bool OrderManagementPage::recordMatchesFilters(const OrderRecord& record) const
{
    const auto contains = [](const QString& value, const QString& keyword) {
        return keyword.isEmpty() || value.contains(keyword, Qt::CaseInsensitive);
    };
    const bool matchesOrder = contains(record.orderNo, orderNumberLineEdit_->text().trimmed());
    const bool matchesUser = contains(record.userName, userLineEdit_->text().trimmed());
    const bool matchesPhone = contains(record.phone, phoneLineEdit_->text().trimmed());
    const bool matchesStation = stationComboBox_->currentIndex() == 0 || record.station == stationComboBox_->currentText();
    const bool matchesCharger = chargerComboBox_->currentIndex() == 0 || record.charger == chargerComboBox_->currentText();
    const bool matchesStatus = statusComboBox_->currentIndex() == 0 || record.status == statusComboBox_->currentText();
    const bool matchesDate = dateRangeComboBox_->currentIndex() == 0 || dateRangeComboBox_->currentIndex() == 2
        || record.startAt.startsWith(QStringLiteral("2025-06-01"));
    return matchesOrder && matchesUser && matchesPhone && matchesStation && matchesCharger && matchesStatus && matchesDate;
}

void OrderManagementPage::applyFilters()
{
    filteredRecordIndexes_.clear();
    for (int index = 0; index < records_.size(); ++index) {
        if (recordMatchesFilters(records_.at(index))) {
            filteredRecordIndexes_.append(index);
        }
    }
    currentPage_ = 0;
    rebuildTable();
    setFeedback(filteredRecordIndexes_.isEmpty() ? tr("未找到符合条件的订单")
                                                  : tr("筛选到 %1 笔本地 Mock 订单").arg(filteredRecordIndexes_.size()));
}

void OrderManagementPage::resetFilters()
{
    orderNumberLineEdit_->clear();
    userLineEdit_->clear();
    phoneLineEdit_->clear();
    stationComboBox_->setCurrentIndex(0);
    chargerComboBox_->setCurrentIndex(0);
    statusComboBox_->setCurrentIndex(0);
    dateRangeComboBox_->setCurrentIndex(0);
    applyFilters();
    setFeedback(tr("已重置筛选条件，显示全部本地 Mock 订单"));
}

void OrderManagementPage::rebuildTable()
{
    const int pageCount = qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = currentPage_ * kPageSize;
    const int end = qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const OrderRecord& record = records_.at(recordIndex);
        const double total = record.chargeFee + record.serviceFee - record.discountFee;
        const QList<QString> values = {record.orderNo, record.userName + tr("\n") + record.phone, record.station,
                                       record.charger + tr("\n") + record.chargerType, record.startAt,
                                       record.duration, QString::number(record.energyKwh, 'f', 2),
                                       tr("¥ %1").arg(QString::number(record.chargeFee, 'f', 2)),
                                       tr("¥ %1").arg(QString::number(record.serviceFee, 'f', 2)),
                                       tr("¥ %1").arg(QString::number(total, 'f', 2)), QString(), QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 10 || column == 11) {
                continue;
            }
            auto* item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, recordIndex);
            item->setTextAlignment(Qt::AlignCenter);
            tableWidget_->setItem(row, column, item);
        }
        tableWidget_->setCellWidget(row, 10, createCompactStatusTag(record.status, tableWidget_));
        auto* detailButton = new QPushButton(tr("详情"), tableWidget_);
        detailButton->setObjectName(QStringLiteral("tableActionButton"));
        detailButton->setAccessibleName(tr("查看订单 %1 的详情").arg(record.orderNo));
        connect(detailButton, &QPushButton::clicked, this, [this, recordIndex]() { showOrderDetails(recordIndex); });
        tableWidget_->setCellWidget(row, 11, createManagementTableCell(detailButton, tableWidget_));
    }
    tableTitleLabel_->setText(tr("订单列表（共 %1 笔）").arg(filteredRecordIndexes_.size()));
    paginationLabel_->setText(tr("第 %1 / %2 页").arg(currentPage_ + 1).arg(pageCount));
    previousPageButton_->setEnabled(currentPage_ > 0);
    nextPageButton_->setEnabled(currentPage_ + 1 < pageCount);
    updateEmptyState();
    if (!filteredRecordIndexes_.isEmpty()) {
        if (!filteredRecordIndexes_.contains(selectedRecordIndex_)) {
            showOrderDetails(filteredRecordIndexes_.first());
        }
    } else {
        selectedRecordIndex_ = -1;
        detailOrderNumberLabel_->setText(tr("暂无匹配订单"));
        detailCreatedAtLabel_->clear();
        detailStatusLabel_->setText(tr("未选择"));
        chargingInfoLabel_->setText(tr("请调整筛选条件后再查看订单详情。"));
        feeInfoLabel_->clear();
        paymentInfoLabel_->clear();
        refreshButton_->setEnabled(false);
    }
}

void OrderManagementPage::updateEmptyState()
{
    const bool isEmpty = filteredRecordIndexes_.isEmpty();
    emptyStateLabel_->setVisible(isEmpty);
    tableWidget_->setVisible(!isEmpty);
    paginationLabel_->setVisible(!isEmpty);
    previousPageButton_->setVisible(!isEmpty);
    nextPageButton_->setVisible(!isEmpty);
}

void OrderManagementPage::showOrderDetails(int recordIndex)
{
    if (recordIndex < 0 || recordIndex >= records_.size()) {
        return;
    }
    selectedRecordIndex_ = recordIndex;
    const OrderRecord& record = records_.at(recordIndex);
    const double total = record.chargeFee + record.serviceFee - record.discountFee;
    detailOrderNumberLabel_->setText(record.orderNo);
    detailStatusLabel_->setText(record.status);
    detailStatusLabel_->setStyleSheet(orderStatusStyle(record.status));
    detailCreatedAtLabel_->setText(tr("创建时间：%1").arg(record.startAt));
    chargingInfoLabel_->setText(
        tr("电站名称　%1\n电桩编号　%2（%3）\n启动时间　%4\n累计结束　%5\n已充时长　%6\n已充电量　%7 kWh\nSOC变化　　32% → 78%")
            .arg(record.station, record.charger, record.chargerType, record.startAt,
                 record.startAt, record.duration, QString::number(record.energyKwh, 'f', 2)));
    feeInfoLabel_->setText(
        tr("充电费　　　¥ %1\n服务费　　　¥ %2\n优惠金额　　¥ %3\n────────────\n实付金额　　¥ %4")
            .arg(QString::number(record.chargeFee, 'f', 2), QString::number(record.serviceFee, 'f', 2),
                 QString::number(record.discountFee, 'f', 2), QString::number(total, 'f', 2)));
    paymentInfoLabel_->setText(tr("支付方式　%1\n支付状态　%2\n交易单号　4200002825202506011289")
                                     .arg(record.paymentMethod, record.paymentStatus));
    refreshButton_->setEnabled(true);
    for (int row = 0; row < tableWidget_->rowCount(); ++row) {
        auto* item = tableWidget_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toInt() == recordIndex) {
            tableWidget_->selectRow(row);
            break;
        }
    }
}

void OrderManagementPage::refreshSelectedOrder()
{
    if (selectedRecordIndex_ < 0 || selectedRecordIndex_ >= records_.size()) {
        return;
    }
    showOrderDetails(selectedRecordIndex_);
    setFeedback(tr("已刷新 %1 的本地 Mock 详情；真实状态需等待 Service 返回。").arg(records_.at(selectedRecordIndex_).orderNo));
}

void OrderManagementPage::showPreviousPage()
{
    if (currentPage_ <= 0) {
        return;
    }
    --currentPage_;
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void OrderManagementPage::showNextPage()
{
    const int pageCount = (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize;
    if (currentPage_ + 1 >= pageCount) {
        return;
    }
    ++currentPage_;
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void OrderManagementPage::setFeedback(const QString& text)
{
    feedbackLabel_->setText(text);
}

} // namespace charging::server
