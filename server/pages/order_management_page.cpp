#include "order_management_page.h"

#include "admin_request_gateway.h"
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
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

namespace charging::server {

namespace {

constexpr int kPageSize = 10;

QString formatCents(qint64 cents)
{
    return QString::number(cents / 100) + QStringLiteral(".")
        + QStringLiteral("%1").arg(cents % 100, 2, 10, QLatin1Char('0'));
}

QString formatKwh(qint64 energyWh)
{
    return QString::number(energyWh / 1000) + QStringLiteral(".")
        + QStringLiteral("%1").arg((energyWh % 1000) / 10, 2, 10, QLatin1Char('0'));
}

QLabel* createTextLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

QString orderStatusText(charging::model::OrderStatus status)
{
    using charging::model::OrderStatus;
    switch (status) {
    case OrderStatus::Charging:
        return QObject::tr("充电中");
    case OrderStatus::WaitingPayment:
        return QObject::tr("待支付");
    case OrderStatus::Completed:
        return QObject::tr("已完成");
    case OrderStatus::Cancelled:
        return QObject::tr("已取消");
    case OrderStatus::Reserved:
        return QObject::tr("已预约");
    }
    return QString();
}

QString orderStatusStyle(charging::model::OrderStatus status)
{
    using charging::model::OrderStatus;
    if (status == OrderStatus::Charging) {
        return QStringLiteral("background:#fff3df; color:#f08a1c; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == OrderStatus::Completed) {
        return QStringLiteral("background:#e8f8f1; color:#20ad86; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == OrderStatus::WaitingPayment) {
        return QStringLiteral("background:#eaf2ff; color:#337df1; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    if (status == OrderStatus::Cancelled) {
        return QStringLiteral("background:#f1f4f8; color:#708096; border-radius:6px; padding:0 7px;"
                              " font-size:12px; font-weight:600;");
    }
    return QStringLiteral("background:#fff0f0; color:#ee5757; border-radius:6px; padding:0 7px;"
                          " font-size:12px; font-weight:600;");
}

QLabel* createStatusTag(charging::model::OrderStatus status, QWidget* parent)
{
    auto* label = new QLabel(orderStatusText(status), parent);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(orderStatusStyle(status));
    return label;
}

QWidget* createCompactStatusTag(charging::model::OrderStatus status, QWidget* parent)
{
    auto* label = createStatusTag(status, nullptr);
    label->setFixedSize(managementStatusTagWidth(label->text()), 26);
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
    setMinimumWidth(kManagementPageMinimumWidth);
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
    orderNumberLineEdit_->setPlaceholderText(tr("订单号、用户或手机号"));
    orderNumberLineEdit_->setMinimumWidth(142);
    userLineEdit_ = new QLineEdit(toolbar);
    userLineEdit_->setPlaceholderText(tr("请输入用户名"));
    userLineEdit_->setMinimumWidth(142);
    phoneLineEdit_ = new QLineEdit(toolbar);
    phoneLineEdit_->setPlaceholderText(tr("请输入手机号"));
    phoneLineEdit_->setMinimumWidth(142);
    stationComboBox_ = new QComboBox(toolbar);
    stationComboBox_->addItems({tr("全部电站"), tr("未来科技城充电站"), tr("滨江智慧园充电站"),
                                tr("城西银泰充电站"), tr("奥体中心充电站"),
                                tr("萧山机场充电站"), tr("富阳智造港充电站")});
    chargerComboBox_ = new QComboBox(toolbar);
    chargerComboBox_->addItems({tr("全部电桩"), tr("CP10010086"), tr("CP10010123"), tr("CP10010205"),
                                tr("CP10010218"), tr("CP10010267"), tr("CP10010345"),
                                tr("CP10010378"), tr("CP10010402"), tr("CP10010495"),
                                tr("CP10010533")});
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
    statusComboBox_->addItem(tr("全部状态"));
    statusComboBox_->addItem(
        tr("充电中"), static_cast<int>(charging::model::OrderStatus::Charging));
    statusComboBox_->addItem(
        tr("待支付"), static_cast<int>(charging::model::OrderStatus::WaitingPayment));
    statusComboBox_->addItem(
        tr("已完成"), static_cast<int>(charging::model::OrderStatus::Completed));
    statusComboBox_->addItem(
        tr("已取消"), static_cast<int>(charging::model::OrderStatus::Cancelled));
    dateRangeComboBox_ = new QComboBox(toolbar);
    dateRangeComboBox_->addItems(
        {tr("全部时间"), tr("演示日（6 月 1 日）"), tr("演示期（近 7 日）")});
    for (auto* comboBox : {statusComboBox_, dateRangeComboBox_}) {
        comboBox->setMinimumWidth(126);
        configureManagementComboBox(comboBox);
    }
    auto* resetButton = new QPushButton(tr("重置"), toolbar);
    resetButton->setObjectName(QStringLiteral("secondaryButton"));
    auto* queryButton = new QPushButton(tr("查询"), toolbar);
    queryButton->setObjectName(QStringLiteral("primaryButton"));
    auto* refreshListButton = new QPushButton(tr("手动刷新"), toolbar);
    refreshListButton->setObjectName(QStringLiteral("secondaryButton"));
    feedbackLabel_ = createTextLabel(tr("显示全部 12,845 笔订单"), QStringLiteral("color:#6f7d92; font-size:13px;"), toolbar);
    feedbackLabel_->setFixedWidth(180);
    feedbackLabel_->setToolTip(feedbackLabel_->text());
    secondLine->addWidget(createTextLabel(tr("订单状态"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    secondLine->addWidget(statusComboBox_);
    secondLine->addWidget(createTextLabel(tr("时间范围"), QStringLiteral("color:#52617a; font-size:13px; font-weight:600;"), toolbar));
    secondLine->addWidget(dateRangeComboBox_);
    secondLine->addWidget(feedbackLabel_);
    secondLine->addStretch();
    secondLine->addWidget(resetButton);
    secondLine->addWidget(queryButton);
    secondLine->addWidget(refreshListButton);
    toolbarLayout->addLayout(secondLine);
    layout->addWidget(toolbar);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);
    auto* tableCard = createCompactCard(this);
    tableCard->setMinimumWidth(kManagementTableMinimumWidth);
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
    tableWidget_->setColumnWidth(10, kManagementStatusColumnWidth);
    tableWidget_->setColumnWidth(11, 72);
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

    auto* detailCard = createManagementDetailCard(tr("订单详情"), this);
    detailCard->setFixedWidth(kManagementDetailWidth);
    auto* detailLayout = qobject_cast<QVBoxLayout*>(detailCard->layout());
    auto* titleRow = new QHBoxLayout();
    detailOrderNumberLabel_ = createTextLabel(QString(), QStringLiteral("color:#273751; font-size:14px; font-weight:700;"), detailCard);
    detailStatusLabel_ = createStatusTag(charging::model::OrderStatus::Charging, detailCard);
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
    auto* paymentDistribution = new QWidget(detailCard);
    paymentDistribution->setObjectName(QStringLiteral("mockPaymentDistribution"));
    auto* paymentRow = new QHBoxLayout(paymentDistribution);
    paymentRow->setContentsMargins(0, 0, 0, 0);
    auto* paymentDonut = new PaymentDonutWidget(detailCard);
    paymentDonut->setObjectName(QStringLiteral("mockPaymentDonut"));
    paymentRow->addWidget(paymentDonut);
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
    detailLayout->addWidget(paymentDistribution);
    detailLayout->addStretch();
    refreshButton_ = new QPushButton(tr("刷新详情"), detailCard);
    refreshButton_->setObjectName(QStringLiteral("secondaryButton"));
    refreshButton_->setAccessibleName(tr("刷新当前订单的本地 Mock 详情"));
    detailLayout->addWidget(refreshButton_);
    contentLayout->addWidget(detailCard);
    layout->addLayout(contentLayout, 1);

    connect(queryButton, &QPushButton::clicked, this, &OrderManagementPage::applyFilters);
    connect(statePanel_, &ManagementStatePanel::resetRequested, this,
            &OrderManagementPage::resetFilters);
    connect(statePanel_, &ManagementStatePanel::retryRequested, this,
            &OrderManagementPage::applyFilters);
    connect(resetButton, &QPushButton::clicked, this, &OrderManagementPage::resetFilters);
    connect(refreshListButton, &QPushButton::clicked, this, &OrderManagementPage::refreshOrderList);
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
    records_ = admin_mock::createOrderRecords();
}

void OrderManagementPage::showLatestOrders()
{
    orderNumberLineEdit_->clear();
    userLineEdit_->clear();
    phoneLineEdit_->clear();
    stationComboBox_->setCurrentIndex(0);
    chargerComboBox_->setCurrentIndex(0);
    statusComboBox_->setCurrentIndex(0);
    dateRangeComboBox_->setCurrentIndex(0);
    applyFilters();
    if (!realMode_) setFeedback(tr("正在显示演示日最新的 %1 笔本地 Mock 订单").arg(filteredRecordIndexes_.size()));
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
    const bool matchesStatus = statusComboBox_->currentIndex() == 0
        || static_cast<int>(record.status) == statusComboBox_->currentData().toInt();
    const bool matchesDate = dateRangeComboBox_->currentIndex() == 0 || dateRangeComboBox_->currentIndex() == 2
        || record.startAt.startsWith(QStringLiteral("2025-06-01"));
    return matchesOrder && matchesUser && matchesPhone && matchesStation && matchesCharger && matchesStatus && matchesDate;
}

void OrderManagementPage::applyFilters()
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
    if (!realMode_) setFeedback(tr("已重置筛选条件，显示全部本地 Mock 订单"));
}

void OrderManagementPage::refreshOrderList()
{
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已于 2025-06-01 10:30:00 刷新本地 Mock 订单；真实结果需等待 Service 返回。"));
}

void OrderManagementPage::rebuildTable()
{
    const int pageCount = realMode_ ? qMax(1, (totalRecords_ + kPageSize - 1) / kPageSize)
                                    : qMax(1, (filteredRecordIndexes_.size() + kPageSize - 1) / kPageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int begin = realMode_ ? 0 : currentPage_ * kPageSize;
    const int end = realMode_ ? filteredRecordIndexes_.size() : qMin(begin + kPageSize, filteredRecordIndexes_.size());
    tableWidget_->setRowCount(end - begin);
    for (int row = 0; row < end - begin; ++row) {
        const int recordIndex = filteredRecordIndexes_.at(begin + row);
        const OrderRecord& record = records_.at(recordIndex);
        const qint64 totalCents = record.chargeFeeCents + record.serviceFeeCents
            - record.discountFeeCents;
        const QList<QString> values = {record.orderNo, record.userName + tr("\n") + record.phone, record.station,
                                       record.charger + tr("\n") + record.chargerType, record.startAt,
                                       record.duration, formatKwh(record.energyWh),
                                       realMode_ ? tr("—") : tr("¥ %1").arg(formatCents(record.chargeFeeCents)),
                                       realMode_ ? tr("—") : tr("¥ %1").arg(formatCents(record.serviceFeeCents)),
                                       tr("¥ %1").arg(formatCents(totalCents)), QString(), QString()};
        for (int column = 0; column < values.size(); ++column) {
            if (column == 10 || column == 11) {
                continue;
            }
            auto* item = createManagementTableItem(values.at(column));
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
    tableTitleLabel_->setText(tr("订单列表（共 %1 笔）").arg(realMode_ ? totalRecords_ : filteredRecordIndexes_.size()));
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
        detailStatusLabel_->setStyleSheet(QStringLiteral("background:#f1f4f8; color:#708096; border-radius:6px;"
                                                         " padding:0 7px; font-size:12px; font-weight:600;"));
        chargingInfoLabel_->setText(tr("请调整筛选条件后再查看订单详情。"));
        feeInfoLabel_->clear();
        paymentInfoLabel_->clear();
        refreshButton_->setEnabled(false);
    }
}

void OrderManagementPage::updateEmptyState()
{
    const bool isEmpty = filteredRecordIndexes_.isEmpty();
    const bool hasFilter = !orderNumberLineEdit_->text().trimmed().isEmpty()
        || statusComboBox_->currentIndex() > 0 || dateRangeComboBox_->currentIndex() > 0;
    const auto state = !isEmpty ? ManagementListState::Hidden
        : realMode_ && !hasFilter ? ManagementListState::EmptyInitial
        : ManagementListState::EmptyFiltered;
    statePanel_->setState(state, realMode_ && !hasFilter
                                     ? tr("服务端当前没有订单记录；用户完成一次充电支付后会显示在这里。")
                                     : tr("当前筛选条件下没有订单。请调整条件或点击“重置”。"));
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
    if (realMode_ && gateway_) {
        detailRequestId_ = gateway_->request(QStringLiteral("orders.get"), {{QStringLiteral("id"), record.serverId}}, this,
                                             QStringLiteral("order-detail"));
    }
    const qint64 totalCents = record.chargeFeeCents + record.serviceFeeCents
        - record.discountFeeCents;
    detailOrderNumberLabel_->setText(record.orderNo);
    detailStatusLabel_->setText(orderStatusText(record.status));
    detailStatusLabel_->setStyleSheet(orderStatusStyle(record.status));
    detailCreatedAtLabel_->setText(tr("创建时间：%1").arg(record.startAt));
    if (realMode_) {
        chargingInfoLabel_->setText(tr("电站名称　%1\n电桩编号　%2\n创建时间　%3\n时长　%4\n电量　%5 kWh\n开始/结束时间、SOC：契约按订单实际字段返回，当前列表未展示")
                                        .arg(record.station, record.charger, record.startAt, record.duration, formatKwh(record.energyWh)));
        feeInfoLabel_->setText(tr("订单金额　¥ %1\n电价快照、费用拆分：当前 DTO 不提供拆分字段").arg(formatCents(totalCents)));
        paymentInfoLabel_->setText(tr("支付信息：契约未提供"));
        refreshButton_->setEnabled(true);
        return;
    }
    chargingInfoLabel_->setText(
        tr("电站名称　%1\n电桩编号　%2（%3）\n启动时间　%4\n累计结束　%5\n已充时长　%6\n已充电量　%7 kWh\nSOC变化　　32% → 78%")
            .arg(record.station, record.charger, record.chargerType, record.startAt,
                 record.startAt, record.duration, formatKwh(record.energyWh)));
    feeInfoLabel_->setText(
        tr("充电费　　　¥ %1\n服务费　　　¥ %2\n优惠金额　　¥ %3\n────────────\n实付金额　　¥ %4")
            .arg(formatCents(record.chargeFeeCents), formatCents(record.serviceFeeCents),
                 formatCents(record.discountFeeCents), formatCents(totalCents)));
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
    if (realMode_) { requestList(); return; }
    showOrderDetails(selectedRecordIndex_);
    setFeedback(tr("已刷新 %1 的本地 Mock 详情；真实状态需等待 Service 返回。").arg(records_.at(selectedRecordIndex_).orderNo));
}

void OrderManagementPage::showPreviousPage()
{
    if (currentPage_ <= 0) {
        return;
    }
    --currentPage_;
    if (realMode_) { requestList(); return; }
    rebuildTable();
    setFeedback(tr("已切换到第 %1 页").arg(currentPage_ + 1));
}

void OrderManagementPage::showNextPage()
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

void OrderManagementPage::setFeedback(const QString& text)
{
    feedbackLabel_->setText(text);
    feedbackLabel_->setToolTip(text);
}

void OrderManagementPage::setAdminGateway(AdminRequestGateway* gateway)
{
    gateway_ = gateway; realMode_ = gateway_ != nullptr;
    if (!gateway_) return;
    stationComboBox_->setEnabled(false); stationComboBox_->setToolTip(tr("当前契约需要站点 ID，列表尚未提供可选项"));
    chargerComboBox_->setEnabled(false); chargerComboBox_->setToolTip(tr("当前契约需要电桩 ID，列表尚未提供可选项"));
    userLineEdit_->setEnabled(false); userLineEdit_->setToolTip(tr("服务端使用统一关键字；请在订单关键字输入框中查询用户或手机号"));
    phoneLineEdit_->setEnabled(false); phoneLineEdit_->setToolTip(tr("服务端使用统一关键字；请在订单关键字输入框中查询用户或手机号"));
    dateRangeComboBox_->setItemText(1, tr("今日（UTC）"));
    dateRangeComboBox_->setItemText(2, tr("近 7 天（UTC）"));
    if (auto* donut = findChild<QWidget*>(QStringLiteral("mockPaymentDonut"))) {
        donut->setVisible(false);
        donut->setToolTip(tr("当前契约不提供支付方式或支付状态分布"));
    }
    if (auto* distribution = findChild<QWidget*>(QStringLiteral("mockPaymentDistribution"))) {
        distribution->setVisible(false);
        distribution->setToolTip(tr("当前契约不提供支付方式或支付状态分布"));
    }
    for (auto* label : findChildren<QLabel*>()) {
        if (label->text() == tr("支付状态分布（今日）")) {
            label->setText(tr("支付状态分布（契约未提供）"));
        }
    }
    connect(gateway_, &AdminRequestGateway::finished, this, [this](const QString& id, const QJsonObject& response) {
        if (id == listRequestId_) handleListResponse(response);
        else if (id == detailRequestId_ && !response.value(QStringLiteral("success")).toBool())
            setFeedback(tr("详情确认失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()));
    });
    connect(gateway_, &AdminRequestGateway::authenticationChanged, this, [this](bool authenticated) {
        if (authenticated) requestList();
    });
    setManagementMetricCardsUnavailable(this, tr("当前契约未提供订单页汇总指标"));
    requestList();
}

void OrderManagementPage::requestList()
{
    if (!gateway_ || !gateway_->isAuthenticated()) return;
    QJsonObject query{{QStringLiteral("page"), currentPage_ + 1}, {QStringLiteral("pageSize"), kPageSize}, {QStringLiteral("sort"), QStringLiteral("createdAtDesc")}};
    const QString keyword = orderNumberLineEdit_->text().trimmed();
    if (!keyword.isEmpty()) query.insert(QStringLiteral("keyword"), keyword);
    const QString statusText = statusComboBox_->currentText();
    if (statusText == tr("充电中")) query.insert(QStringLiteral("status"), QStringLiteral("CHARGING"));
    else if (statusText == tr("待支付")) query.insert(QStringLiteral("status"), QStringLiteral("WAITING_PAYMENT"));
    else if (statusText == tr("已完成")) query.insert(QStringLiteral("status"), QStringLiteral("COMPLETED"));
    else if (statusText == tr("已取消")) query.insert(QStringLiteral("status"), QStringLiteral("CANCELLED"));
    if (dateRangeComboBox_->currentIndex() > 0) {
        const auto now = QDateTime::currentDateTimeUtc(); QDate from = now.date();
        if (dateRangeComboBox_->currentIndex() == 2) from = from.addDays(-6);
        else if (dateRangeComboBox_->currentIndex() == 3) from = QDate(from.year(), from.month(), 1);
        query.insert(QStringLiteral("createdAtFrom"), QDateTime(from, QTime(0,0), Qt::UTC).toString(Qt::ISODateWithMs));
        query.insert(QStringLiteral("createdAtTo"), QDateTime(now.date().addDays(1), QTime(0,0), Qt::UTC).toString(Qt::ISODateWithMs));
    }
    listRequestId_ = gateway_->request(QStringLiteral("orders.list"), query, this, QStringLiteral("order-list")); setFeedback(tr("正在加载服务数据…"));
}

void OrderManagementPage::handleListResponse(const QJsonObject& response)
{
    records_.clear(); filteredRecordIndexes_.clear(); selectedRecordIndex_ = -1;
    if (!response.value(QStringLiteral("success")).toBool()) { totalRecords_ = 0; rebuildTable(); setFeedback(tr("加载失败：%1").arg(response.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString())); return; }
    const auto data = response.value(QStringLiteral("data")).toObject(); totalRecords_ = data.value(QStringLiteral("total")).toInt();
    for (const auto& value : data.value(QStringLiteral("items")).toArray()) {
        const auto i = value.toObject(); const auto code = i.value(QStringLiteral("status")).toString();
        const auto status = code == QStringLiteral("CHARGING") ? charging::model::OrderStatus::Charging : code == QStringLiteral("WAITING_PAYMENT") ? charging::model::OrderStatus::WaitingPayment : code == QStringLiteral("COMPLETED") ? charging::model::OrderStatus::Completed : code == QStringLiteral("CANCELLED") ? charging::model::OrderStatus::Cancelled : charging::model::OrderStatus::Reserved;
        const qint64 amount = i.value(QStringLiteral("amountCents")).toInteger();
        records_.append({i.value(QStringLiteral("orderNo")).toString(), i.value(QStringLiteral("nickname")).toString(), i.value(QStringLiteral("phone")).toString(),
            i.value(QStringLiteral("stationName")).toString(), i.value(QStringLiteral("chargerCode")).toString(), tr("契约未提供"), status,
            i.value(QStringLiteral("createdAt")).toString(), tr("%1 分钟").arg(i.value(QStringLiteral("durationSeconds")).toInt() / 60),
            i.value(QStringLiteral("energyWh")).toInteger(), amount, 0, 0, tr("契约未提供"), tr("契约未提供"), i.value(QStringLiteral("id")).toString()});
        filteredRecordIndexes_.append(records_.size() - 1);
    }
    rebuildTable(); setFeedback(totalRecords_ ? tr("已加载 %1 笔订单（服务端分页）").arg(totalRecords_) : tr("当前没有订单数据"));
}

} // namespace charging::server
