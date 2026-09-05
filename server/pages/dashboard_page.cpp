#include "dashboard_page.h"

#include "admin_request_gateway.h"
#include "dashboard_visual_widgets.h"
#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

namespace charging::server {

namespace {

QLabel* makeLabel(const QString& text, const QString& style, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(style);
    return label;
}

QFrame* createMetricCard(const QString& title, const QString& value, const QString& unit,
                         const QString& comparison, const QColor& accent, int iconType,
                         QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("summaryCard"));
    card->setMinimumHeight(132);
    card->setMinimumWidth(220);
    auto* layout = new QHBoxLayout(card);
    layout->setContentsMargins(22, 20, 20, 18);
    layout->setSpacing(16);
    layout->addWidget(new MetricIconWidget(accent, iconType, card), 0, Qt::AlignTop);
    auto* textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(5);
    textLayout->addWidget(
        makeLabel(title, QStringLiteral("color:#627089; font-size:13px;"), card));
    textLayout->addWidget(makeLabel(value + unit,
                                    QStringLiteral("color:#1b2a45; font-size:26px; font-weight:700;"),
                                    card));
    textLayout->addWidget(
        makeLabel(comparison, QStringLiteral("color:#68758a; font-size:13px;"), card));
    layout->addLayout(textLayout, 1);
    return card;
}

QPushButton* createPeriodButton(const QString& title, bool active, QWidget* parent)
{
    auto* button = new QPushButton(title, parent);
    button->setMinimumHeight(38);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(active
                              ? QStringLiteral("QPushButton { background:#ffffff; "
                                               "border:1px solid #dbe7fa; border-radius:8px; "
                                               "color:#2878f0; padding:0 13px; font-size:14px; }")
                              : QStringLiteral("QPushButton { background:#f6f8fb; border:none; "
                                               "border-radius:8px; color:#78859a; padding:0 13px; font-size:14px; }"));
    return button;
}

void setPeriodButtonActive(QPushButton* button, bool active)
{
    button->setStyleSheet(active
                              ? QStringLiteral("QPushButton { background:#ffffff; "
                                               "border:1px solid #dbe7fa; border-radius:8px; "
                                               "color:#2878f0; padding:0 13px; font-size:14px; }")
                              : QStringLiteral("QPushButton { background:#f6f8fb; border:none; "
                                               "border-radius:8px; color:#78859a; padding:0 13px; font-size:14px; }"));
}

void setMetricTabActive(QPushButton* button, bool active)
{
    button->setStyleSheet(active
                              ? QStringLiteral("QPushButton { background:#347cf6; color:white; "
                                               "border:none; border-radius:7px; min-height:38px; "
                                               "padding:0 12px; font-size:14px; }")
                              : QStringLiteral("QPushButton { background:#f3f6fa; color:#607087; "
                                               "border:none; border-radius:7px; min-height:38px; "
                                               "padding:0 12px; font-size:14px; }"));
}

QTableWidget* createDashboardTable(const QStringList& headers, QWidget* parent)
{
    auto* table = new QTableWidget(parent);
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setShowGrid(false);
    table->setFocusPolicy(Qt::NoFocus);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    // Dashboard is a compact five-row summary.  Its rows are intentionally
    // denser than management tables so the complete list fits without a scroll bar.
    table->verticalHeader()->setDefaultSectionSize(40);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    return table;
}

void setRow(QTableWidget* table, int row, const QStringList& values, int coloredColumn = -1,
            const QColor& color = QColor())
{
    table->insertRow(row);
    for (int column = 0; column < values.size(); ++column) {
        auto* item = createManagementTableItem(values.at(column));
        item->setForeground(column == coloredColumn ? color : QColor("#40506a"));
        item->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, column, item);
    }
}

void setEmptyRow(QTableWidget* table, const QString& message)
{
    table->setRowCount(1);
    table->setSpan(0, 0, 1, table->columnCount());
    auto* item = createManagementTableItem(message);
    item->setTextAlignment(Qt::AlignCenter);
    item->setForeground(QColor("#718098"));
    table->setItem(0, 0, item);
}

QFrame* createTableCard(const QString& title, const QString& badge, const QString& footer,
                        QLabel** badgeLabel, QPushButton** footerButton, QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("contentCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 12);
    layout->setSpacing(7);
    auto* header = new QHBoxLayout();
    header->addWidget(
        makeLabel(title, QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"), card));
    if (!badge.isEmpty()) {
        auto* badgeWidget = makeLabel(
            badge, QStringLiteral("background:#ff4e4e; color:white; font-size:13px; "
                                  "font-weight:700; border-radius:10px; padding:2px 7px;"), card);
        header->addWidget(badgeWidget);
        if (badgeLabel != nullptr) {
            *badgeLabel = badgeWidget;
        }
    }
    header->addStretch();
    layout->addLayout(header);
    if (!footer.isEmpty()) {
        auto* link = new QPushButton(footer, card);
        link->setFlat(true);
        link->setCursor(Qt::PointingHandCursor);
        link->setStyleSheet(QStringLiteral(
            "QPushButton { color:#2878f0; border:none; padding:3px 6px; font-size:13px; }"
            "QPushButton:hover { color:#1769e8; text-decoration:underline; }"));
        if (footerButton != nullptr) {
            *footerButton = link;
        }
        layout->addWidget(link, 0, Qt::AlignHCenter);
    }
    return card;
}

} // namespace

DashboardPage::DashboardPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("dashboardPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);

    auto* summaryLayout = new QHBoxLayout();
    summaryLayout->setSpacing(16);
    auto* todayCard = createMetricCard(
        tr("今日营收"), tr("¥ —"), QString(),
        tr("服务数据加载后显示（UTC）"), QColor("#347cf6"), 0, this);
    todayRevenueValue_ = todayCard->findChildren<QLabel*>().at(1);
    summaryLayout->addWidget(todayCard);
    auto* monthCard = createMetricCard(
        tr("本月营收"), tr("¥ —"), QString(),
        tr("服务数据加载后显示（UTC）"), QColor("#43c7bc"), 1, this);
    monthRevenueValue_ = monthCard->findChildren<QLabel*>().at(1);
    summaryLayout->addWidget(monthCard);
    auto* onlineCard = createMetricCard(
        tr("在线电桩"), tr("—"), QString(),
        tr("在线率待加载"), QColor("#347cf6"), 2, this);
    const auto onlineLabels = onlineCard->findChildren<QLabel*>();
    onlineChargersValue_ = onlineLabels.at(1);
    onlineChargersHint_ = onlineLabels.at(2);
    summaryLayout->addWidget(onlineCard);
    summaryLayout->addWidget(createMetricCard(
        tr("当前连接"), tr("0"), tr(" 个"),
        tr("服务连接数"), QColor("#43c7bc"), 3, this));
    clientCountValue_ = summaryLayout->itemAt(3)->widget()->findChildren<QLabel*>().at(1);
    layout->addLayout(summaryLayout);

    auto* dataLayout = new QHBoxLayout();
    dataLayout->setSpacing(18);
    auto* trendCard = new QFrame(this);
    trendCard->setObjectName(QStringLiteral("contentCard"));
    trendCard->setMinimumHeight(338);
    auto* trendLayout = new QVBoxLayout(trendCard);
    trendLayout->setContentsMargins(18, 14, 18, 13);
    trendLayout->setSpacing(5);
    auto* trendHeader = new QHBoxLayout();
    trendHeader->addWidget(makeLabel(
        tr("营收趋势  ⓘ"), QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"),
        trendCard));
    trendHeader->addStretch();
    auto* todayButton = createPeriodButton(tr("今日"), false, trendCard);
    auto* weekButton = createPeriodButton(tr("近7天"), true, trendCard);
    auto* monthButton = createPeriodButton(tr("近30天"), false, trendCard);
    auto* customButton = createPeriodButton(tr("自定义"), false, trendCard);
    todayButton->setEnabled(false);
    todayButton->setToolTip(tr("当前契约仅提供近 7 天或近 30 天趋势"));
    customButton->setEnabled(false);
    customButton->setToolTip(tr("当前契约不提供自定义日期范围趋势"));
    trendHeader->addWidget(todayButton);
    trendHeader->addWidget(weekButton);
    trendHeader->addWidget(monthButton);
    trendHeader->addWidget(customButton);
    trendLayout->addLayout(trendHeader);
    auto* measureTabs = new QHBoxLayout();
    auto* revenueTab = new QPushButton(tr("营收金额"), trendCard);
    auto* orderTab = new QPushButton(tr("完成订单"), trendCard);
    setMetricTabActive(revenueTab, true);
    setMetricTabActive(orderTab, false);
    measureTabs->addWidget(revenueTab);
    measureTabs->addWidget(orderTab);
    measureTabs->addStretch();
    trendLayout->addLayout(measureTabs);
    auto* trendWidget = new RevenueTrendWidget(trendCard);
    trendWidget_ = trendWidget;
    trendWidget_->setServiceSeries({}, {}, {});
    trendLayout->addWidget(trendWidget, 1);
    const QList<QPushButton*> periodButtons = {todayButton, weekButton, monthButton, customButton};
    for (int index = 0; index < 3; ++index) {
        connect(periodButtons.at(index), &QPushButton::clicked, this,
                [periodButtons, trendWidget, index]() {
                    for (int buttonIndex = 0; buttonIndex < periodButtons.size(); ++buttonIndex) {
                        setPeriodButtonActive(periodButtons.at(buttonIndex), buttonIndex == index);
                    }
                    trendWidget->setPeriod(index);
                });
    }
    connect(weekButton, &QPushButton::clicked, this, [this] { refresh(7); });
    connect(monthButton, &QPushButton::clicked, this, [this] { refresh(30); });
    connect(customButton, &QPushButton::clicked, this, [this, periodButtons, trendWidget]() {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("自定义营收趋势时间"));
        dialog.setMinimumWidth(360);
        auto* dialogLayout = new QVBoxLayout(&dialog);
        auto* formLayout = new QFormLayout();
        formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* startDateEdit = new QDateEdit(QDate(2025, 5, 18), &dialog);
        auto* endDateEdit = new QDateEdit(QDate(2025, 5, 31), &dialog);
        for (auto* dateEdit : {startDateEdit, endDateEdit}) {
            dateEdit->setCalendarPopup(true);
            dateEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
            dateEdit->setDateRange(QDate(2025, 1, 1), QDate(2025, 12, 31));
        }
        formLayout->addRow(tr("开始日期："), startDateEdit);
        formLayout->addRow(tr("结束日期："), endDateEdit);
        dialogLayout->addLayout(formLayout);
        auto* hint = makeLabel(tr("请选择 2 至 31 天；确认后图表会按每日数据重新绘制。"),
                               QStringLiteral("color:#718098; font-size:13px;"), &dialog);
        hint->setWordWrap(true);
        dialogLayout->addWidget(hint);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("应用"));
        buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
        dialogLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, startDateEdit, endDateEdit]() {
            const int dayCount = startDateEdit->date().daysTo(endDateEdit->date()) + 1;
            if (dayCount < 2 || dayCount > 31) {
                QMessageBox::warning(&dialog, QObject::tr("日期范围无效"),
                                     QObject::tr("请选择连续 2 至 31 天，且结束日期不得早于开始日期。"));
                return;
            }
            dialog.accept();
        });
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        for (int buttonIndex = 0; buttonIndex < periodButtons.size(); ++buttonIndex) {
            setPeriodButtonActive(periodButtons.at(buttonIndex), buttonIndex == 3);
        }
        trendWidget->setCustomDateRange(startDateEdit->date(), endDateEdit->date());
    });
    connect(revenueTab, &QPushButton::clicked, this, [revenueTab, orderTab, trendWidget]() {
        setMetricTabActive(revenueTab, true);
        setMetricTabActive(orderTab, false);
        trendWidget->setDisplayMode(0);
    });
    connect(orderTab, &QPushButton::clicked, this, [revenueTab, orderTab, trendWidget]() {
        setMetricTabActive(revenueTab, false);
        setMetricTabActive(orderTab, true);
        trendWidget->setDisplayMode(1);
    });
    dataLayout->addWidget(trendCard, 7);

    auto* deviceCard = new QFrame(this);
    deviceCard->setObjectName(QStringLiteral("contentCard"));
    deviceCard->setMinimumWidth(360);
    auto* deviceLayout = new QVBoxLayout(deviceCard);
    deviceLayout->setContentsMargins(18, 16, 18, 12);
    deviceLayout->addWidget(makeLabel(
        tr("设备状态"), QStringLiteral("color:#1d2c46; font-size:18px; font-weight:700;"),
        deviceCard));
    auto* distribution = new QHBoxLayout();
    distribution->setSpacing(12);
    deviceStatusWidget_ = new DeviceStatusWidget(deviceCard);
    distribution->addWidget(deviceStatusWidget_, 0, Qt::AlignCenter);
    auto* legend = new QVBoxLayout();
    const struct {
        const char* name;
        const char* data;
        const char* color;
    } statuses[] = {
        {"在线", "—", "#43c7bc"},
        {"离线", "—", "#aab4c2"},
        {"故障", "—", "#f5a130"},
    };
    int statusIndex = 0;
    for (const auto& status : statuses) {
        auto* row = new QHBoxLayout();
        auto* dot = new QLabel(deviceCard);
        dot->setFixedSize(9, 9);
        dot->setStyleSheet(
            QStringLiteral("background:%1; border-radius:4px;").arg(QString::fromLatin1(status.color)));
        row->addWidget(dot);
        row->addWidget(makeLabel(QString::fromUtf8(status.name),
                                 QStringLiteral("color:#536178; font-size:13px;"), deviceCard));
        row->addStretch();
        auto* value = makeLabel(QString::fromUtf8(status.data),
                                QStringLiteral("color:#3c4c67; font-size:13px;"), deviceCard);
        if (statusIndex == 0) onlineLegendValue_ = value;
        else if (statusIndex == 1) offlineLegendValue_ = value;
        else faultLegendValue_ = value;
        ++statusIndex;
        row->addWidget(value);
        legend->addLayout(row);
    }
    legend->addStretch();
    distribution->addLayout(legend, 1);
    deviceLayout->addLayout(distribution, 1);
    auto* deviceFooter = new QHBoxLayout();
    totalChargersLabel_ = makeLabel(tr("总电桩数：—"),
                                      QStringLiteral("color:#8490a4; font-size:13px;"), deviceCard);
    deviceFooter->addWidget(totalChargersLabel_);
    deviceFooter->addStretch();
    refreshedAtLabel_ = makeLabel(tr("刷新时间：等待服务响应"),
                                       QStringLiteral("color:#8490a4; font-size:13px;"), deviceCard);
    auto* refreshButton = new QPushButton(tr("刷新概览"), deviceCard);
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#ffffff; color:#2878f0; border:1px solid #dfe6f0; border-radius:7px;"
        " min-height:30px; padding:0 9px; font-size:13px; }"
        "QPushButton:hover { background:#f6f9ff; }"));
    refreshButton->setAccessibleName(tr("刷新运营概览"));
    refreshButton_ = refreshButton;
    // QPushButton::clicked carries a bool.  Passing it directly to refresh(int)
    // turned a normal click into days=0/1, which the service correctly rejects.
    connect(refreshButton, &QPushButton::clicked, this,
            [this]() { refresh(requestedDays_); });
    deviceFooter->addWidget(refreshedAtLabel_);
    deviceFooter->addWidget(refreshButton);
    deviceLayout->addLayout(deviceFooter);
    dataLayout->addWidget(deviceCard, 3);
    layout->addLayout(dataLayout);

    auto* tableLayout = new QHBoxLayout();
    tableLayout->setSpacing(18);
    QLabel* exceptionCountBadge = nullptr;
    QPushButton* exceptionListButton = nullptr;
    auto* exceptionCard = createTableCard(
        tr("异常电桩"), tr("5"), tr("查看全部异常  ›"), &exceptionCountBadge,
        &exceptionListButton, this);
    auto* exceptionLayout = qobject_cast<QVBoxLayout*>(exceptionCard->layout());
    auto* exceptionTable = createDashboardTable(
        {tr("电桩编号"), tr("所属电站"), tr("异常类型"), tr("记录更新时间"), tr("操作")}, exceptionCard);
    exceptionTable->horizontalHeader()->setStretchLastSection(false);
    exceptionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    exceptionTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    exceptionTable->setColumnWidth(4, 84);
    exceptionCountBadge->setText(QStringLiteral("—"));
    exceptionCountBadge_ = exceptionCountBadge;
    exceptionTable_ = exceptionTable;
    exceptionListButton->setAccessibleName(tr("查看全部异常电桩"));
    connect(exceptionListButton, &QPushButton::clicked, this,
            [this]() { emit exceptionListRequested(); });
    exceptionLayout->insertWidget(1, exceptionTable, 1);
    tableLayout->addWidget(exceptionCard, 1);

    QPushButton* latestOrdersButton = nullptr;
    auto* recentOrderCard = createTableCard(
        tr("最新订单"), QString(), tr("查看全部订单  ›"), nullptr, &latestOrdersButton, this);
    auto* orderLayout = qobject_cast<QVBoxLayout*>(recentOrderCard->layout());
    auto* orderTable = createDashboardTable(
        {tr("订单号"), tr("用户"), tr("电站"), tr("金额"), tr("状态"), tr("时间")}, recentOrderCard);
    orderTable->horizontalHeader()->setStretchLastSection(false);
    // The card is intentionally wider than this compact six-column summary.
    // Stretch the data columns together so spare width becomes readable space,
    // rather than leaving an empty block to the right of the time column.
    for (int column : {0, 1, 2, 3, 5}) {
        orderTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
    }
    orderTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    orderTable->setColumnWidth(4, 76);
    /*const auto latestOrders = admin_mock::createOrderRecords();
    for (int row = 0; row < 5 && row < latestOrders.size(); ++row) {
        const auto& record = latestOrders.at(row);
        const qint64 amountCents = record.chargeFeeCents + record.serviceFeeCents
            - record.discountFeeCents;
        const QString statusText = dashboardOrderStatus(record.status);
        setRow(orderTable, row,
               {record.orderNo, record.userName, record.station,
                tr("¥ %1").arg(formatCents(amountCents)), statusText,
                record.startAt.mid(11, 5)});
        auto* state = new QLabel(statusText, orderTable);
        state->setAlignment(Qt::AlignCenter);
        state->setMinimumSize(62, 26);
        state->setMaximumHeight(26);
        state->setStyleSheet(record.status == charging::model::OrderStatus::Completed
                                 ? QStringLiteral("background:#e8f8f0; color:#28a86f; "
                                                  "border-radius:6px; padding:0 7px; font-size:12px;")
                                 : QStringLiteral("background:#eaf3ff; color:#2878f0; "
                                                  "border-radius:6px; padding:0 7px; font-size:12px;"));
        orderTable->setCellWidget(row, 4, createManagementTableCell(state, orderTable));
    }*/
    latestOrdersTable_ = orderTable;
    latestOrdersButton->setAccessibleName(tr("查看最新订单"));
    connect(latestOrdersButton, &QPushButton::clicked, this,
            [this]() { emit latestOrdersRequested(); });
    orderLayout->insertWidget(1, orderTable, 1);
    tableLayout->addWidget(recentOrderCard, 1);
    layout->addLayout(tableLayout);
}

void DashboardPage::setClientCount(int count)
{
    if (clientCountValue_ != nullptr) clientCountValue_->setText(QString::number(count) + tr(" 个"));
}

void DashboardPage::setAdminGateway(AdminRequestGateway* gateway)
{
    gateway_ = gateway;
    if (gateway_ == nullptr) return;
    connect(gateway_, &AdminRequestGateway::finished, this,
            [this](const QString& id, const QJsonObject& response) {
                if (id == requestId_) handleDashboardResponse(response);
            });
    if (gateway_->isAuthenticated()) refresh();
}

void DashboardPage::refresh(int days)
{
    if (gateway_ == nullptr) return;
    requestedDays_ = days;
    refreshButton_->setEnabled(false);
    refreshedAtLabel_->setText(tr("刷新时间：正在加载服务数据…"));
    requestId_ = gateway_->request(QStringLiteral("dashboard.get"), {{QStringLiteral("days"), days}}, this,
                                   QStringLiteral("dashboard"));
}

void DashboardPage::handleDashboardResponse(const QJsonObject& response)
{
    refreshButton_->setEnabled(true);
    if (!response.value(QStringLiteral("success")).toBool()) {
        refreshedAtLabel_->setText(tr("刷新失败：%1").arg(response.value("error").toObject().value("message").toString()));
        return;
    }
    const auto data = response.value(QStringLiteral("data")).toObject();
    const auto cents = [](qint64 value) { return QStringLiteral("¥ %1.%2").arg(value / 100).arg(value % 100, 2, 10, QLatin1Char('0')); };
    todayRevenueValue_->setText(cents(data.value("todayRevenueCents").toInteger()));
    monthRevenueValue_->setText(cents(data.value("monthRevenueCents").toInteger()));
    const qint64 online = data.value("availableChargers").toInteger() + data.value("reservedChargers").toInteger() + data.value("chargingChargers").toInteger();
    onlineChargersValue_->setText(QString::number(online) + tr(" 台"));
    onlineChargersHint_->setText(tr("在线率 %1%（UTC 快照）").arg(data.value("onlineRatio").toDouble() * 100, 0, 'f', 1));
    const int offline = data.value("offlineChargers").toInt();
    const int fault = data.value("faultChargers").toInt();
    if (deviceStatusWidget_) deviceStatusWidget_->setCounts(online, offline, fault);
    const int total = data.value("totalChargers").toInt();
    const auto legend = [total](int count) { return total ? QObject::tr("%1（%2%）").arg(count).arg(100.0 * count / total, 0, 'f', 1) : QObject::tr("0（0.0%）"); };
    if (onlineLegendValue_) onlineLegendValue_->setText(legend(online));
    if (offlineLegendValue_) offlineLegendValue_->setText(legend(offline));
    if (faultLegendValue_) faultLegendValue_->setText(legend(fault));
    totalChargersLabel_->setText(tr("总电桩数：%1 台").arg(data.value("totalChargers").toInteger()));
    refreshedAtLabel_->setText(tr("刷新时间：%1（UTC）").arg(data.value("observedAt").toString()));
    QStringList dates; QVector<qint64> revenue; QVector<int> orders;
    for (const auto& pointValue : data.value("trend").toArray()) {
        const auto point = pointValue.toObject();
        dates << point.value("date").toString();
        revenue << point.value("revenueCents").toInteger();
        orders << point.value("completedOrderCount").toInt();
    }
    trendWidget_->setServiceSeries(dates, revenue, orders);
    const auto abnormalities = data.value("abnormalChargers").toObject();
    exceptionCountBadge_->setText(QString::number(abnormalities.value("total").toInteger()));
    exceptionTable_->setRowCount(0);
    const auto abnormalItems = abnormalities.value("items").toArray();
    for (const auto& value : abnormalItems) {
        const auto item = value.toObject(); const int row = exceptionTable_->rowCount();
        const auto exception = item.value("exceptionType").toString() == QStringLiteral("FAULT") ? tr("故障") : tr("离线");
        setRow(exceptionTable_, row, {item.value("code").toString(), item.value("stationName").toString(),
               exception, item.value("updatedAt").toString(), tr("请至电桩管理处理")});
    }
    if (abnormalItems.isEmpty()) {
        setEmptyRow(exceptionTable_, tr("当前没有异常电桩（服务端实时数据）"));
    }
    latestOrdersTable_->setRowCount(0);
    const auto latestOrderItems = data.value("latestOrders").toObject().value("items").toArray();
    for (const auto& value : latestOrderItems) {
        const auto item = value.toObject(); const int row = latestOrdersTable_->rowCount();
        const auto code = item.value("status").toString();
        const auto status = code == QStringLiteral("CHARGING") ? tr("充电中") : code == QStringLiteral("WAITING_PAYMENT") ? tr("待支付") : code == QStringLiteral("COMPLETED") ? tr("已完成") : code == QStringLiteral("CANCELLED") ? tr("已取消") : tr("已预约");
        setRow(latestOrdersTable_, row, {item.value("orderNo").toString(), item.value("phone").toString(),
               item.value("stationName").toString(), cents(item.value("amountCents").toInteger()),
               status, item.value("createdAt").toString()});
    }
    if (latestOrderItems.isEmpty()) {
        setEmptyRow(latestOrdersTable_, tr("当前数据库暂无订单；完成一次用户端充电支付后会显示在这里"));
    }
}

} // namespace charging::server
