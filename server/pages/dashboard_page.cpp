#include "dashboard_page.h"

#include "dashboard_visual_widgets.h"
#include "management_page_widgets.h"

#include <QAbstractItemView>
#include <QFrame>
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
        auto* item = new QTableWidgetItem(values.at(column));
        item->setForeground(column == coloredColumn ? color : QColor("#40506a"));
        item->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, column, item);
    }
}

QFrame* createTableCard(const QString& title, const QString& badge, const QString& footer,
                        QLabel** badgeLabel, QWidget* parent)
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
    header->addWidget(
        makeLabel(QStringLiteral("⋮"), QStringLiteral("color:#2b4264; font-size:22px;"), card));
    layout->addLayout(header);
    if (!footer.isEmpty()) {
        auto* footerLabel = makeLabel(
            footer, QStringLiteral("color:#2878f0; font-size:13px;"), card);
        footerLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(footerLabel);
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
    summaryLayout->addWidget(createMetricCard(
        tr("今日营收"), tr("¥ 128,560.00"), QString(),
        tr("较昨日  <span style='color:#2878f0'>+12.6% ↑</span>"), QColor("#347cf6"), 0, this));
    summaryLayout->addWidget(createMetricCard(
        tr("本月营收"), tr("¥ 3,245,670.00"), QString(),
        tr("较上月  <span style='color:#2878f0'>+8.4% ↑</span>"), QColor("#43c7bc"), 1, this));
    summaryLayout->addWidget(createMetricCard(
        tr("在线电桩"), tr("1,256"), tr(" 台"),
        tr("在线率  <span style='color:#35bfb4'>82.4%</span>"), QColor("#347cf6"), 2, this));
    summaryLayout->addWidget(createMetricCard(
        tr("当前连接"), tr("512"), tr(" 辆"),
        tr("较昨日  <span style='color:#35bfb4'>+9.7%</span>"), QColor("#43c7bc"), 3, this));
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
    auto* weekButton = createPeriodButton(tr("近7天"), false, trendCard);
    auto* monthButton = createPeriodButton(tr("近30天"), true, trendCard);
    auto* customButton = createPeriodButton(tr("自定义"), false, trendCard);
    trendHeader->addWidget(todayButton);
    trendHeader->addWidget(weekButton);
    trendHeader->addWidget(monthButton);
    trendHeader->addWidget(customButton);
    trendHeader->addWidget(
        makeLabel(tr("⋮"), QStringLiteral("color:#526179; font-size:22px;"), trendCard));
    trendLayout->addLayout(trendHeader);
    auto* measureTabs = new QHBoxLayout();
    auto* revenueTab = new QPushButton(tr("营收金额"), trendCard);
    auto* orderTab = new QPushButton(tr("订单金额"), trendCard);
    setMetricTabActive(revenueTab, true);
    setMetricTabActive(orderTab, false);
    measureTabs->addWidget(revenueTab);
    measureTabs->addWidget(orderTab);
    measureTabs->addStretch();
    trendLayout->addLayout(measureTabs);
    auto* trendWidget = new RevenueTrendWidget(trendCard);
    trendLayout->addWidget(trendWidget, 1);
    const QList<QPushButton*> periodButtons = {todayButton, weekButton, monthButton, customButton};
    for (int index = 0; index < periodButtons.size(); ++index) {
        connect(periodButtons.at(index), &QPushButton::clicked, this,
                [periodButtons, trendWidget, index]() {
                    for (int buttonIndex = 0; buttonIndex < periodButtons.size(); ++buttonIndex) {
                        setPeriodButtonActive(periodButtons.at(buttonIndex), buttonIndex == index);
                    }
                    trendWidget->setPeriod(index);
                });
    }
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
    distribution->addWidget(new DeviceStatusWidget(deviceCard), 0, Qt::AlignCenter);
    auto* legend = new QVBoxLayout();
    const struct {
        const char* name;
        const char* data;
        const char* color;
    } statuses[] = {
        {"在线", "1,256（82.4%）", "#43c7bc"},
        {"离线", "178（11.7%）", "#aab4c2"},
        {"故障", "88（5.8%）", "#f5a130"},
    };
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
        row->addWidget(makeLabel(QString::fromUtf8(status.data),
                                 QStringLiteral("color:#3c4c67; font-size:13px;"), deviceCard));
        legend->addLayout(row);
    }
    legend->addStretch();
    distribution->addLayout(legend, 1);
    deviceLayout->addLayout(distribution, 1);
    auto* deviceFooter = new QHBoxLayout();
    deviceFooter->addWidget(makeLabel(tr("总电桩数：1,522 台"),
                                      QStringLiteral("color:#8490a4; font-size:13px;"), deviceCard));
    deviceFooter->addStretch();
    deviceFooter->addWidget(makeLabel(tr("刷新时间：06-01 10:30:00  ⟳"),
                                      QStringLiteral("color:#8490a4; font-size:13px;"), deviceCard));
    deviceLayout->addLayout(deviceFooter);
    dataLayout->addWidget(deviceCard, 3);
    layout->addLayout(dataLayout);

    auto* tableLayout = new QHBoxLayout();
    tableLayout->setSpacing(18);
    QLabel* exceptionCountBadge = nullptr;
    auto* exceptionCard = createTableCard(
        tr("异常电桩"), tr("5"), tr("查看全部异常  ›"), &exceptionCountBadge, this);
    auto* exceptionLayout = qobject_cast<QVBoxLayout*>(exceptionCard->layout());
    auto* exceptionTable = createDashboardTable(
        {tr("电桩编号"), tr("所属电站"), tr("异常类型"), tr("异常时间"), tr("操作")}, exceptionCard);
    exceptionTable->horizontalHeader()->setStretchLastSection(false);
    exceptionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    exceptionTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    exceptionTable->setColumnWidth(4, 84);
    const QStringList exceptionRows[] = {
        {tr("CP10010086"), tr("未来科技城充电站"), tr("充电枪通讯异常"), tr("2025-06-01 09:58"), tr("处理")},
        {tr("CP10010123"), tr("滨江智慧园充电站"), tr("充电中断"), tr("2025-06-01 09:41"), tr("处理")},
        {tr("CP10010205"), tr("城西银泰充电站"), tr("过温保护"), tr("2025-06-01 09:22"), tr("处理")},
        {tr("CP10010218"), tr("奥体中心充电站"), tr("模块故障"), tr("2025-06-01 09:10"), tr("处理")},
        {tr("CP10010267"), tr("萧山机场充电站"), tr("离线"), tr("2025-06-01 08:55"), tr("处理")},
    };
    for (int row = 0; row < 5; ++row) {
        const QColor typeColor = row == 4 ? QColor("#748196")
            : (row == 1 ? QColor("#f09b24") : QColor("#ff4b4b"));
        setRow(exceptionTable, row, exceptionRows[row], 2, typeColor);
        auto* action = new QPushButton(tr("处理"), exceptionTable);
        action->setObjectName(QStringLiteral("tableActionButton"));
        exceptionTable->setCellWidget(row, 4, createManagementTableCell(action, exceptionTable));
        connect(action, &QPushButton::clicked, this,
                [this, exceptionTable, exceptionCountBadge, action, row]() {
                    const QString chargerCode = exceptionTable->item(row, 0)->text();
                    const auto choice = QMessageBox::question(
                        this, tr("确认处理异常"),
                        tr("确认将电桩 %1 的异常标记为已处理吗？该操作仅更新本地 Mock 状态。")
                            .arg(chargerCode));
                    if (choice != QMessageBox::Yes) {
                        return;
                    }

                    exceptionTable->item(row, 2)->setText(tr("已处理"));
                    exceptionTable->item(row, 2)->setForeground(QColor("#6f7d90"));
                    action->setText(tr("已处理"));
                    action->setEnabled(false);
                    int remainingCount = 0;
                    for (int index = 0; index < exceptionTable->rowCount(); ++index) {
                        auto* actionCell = exceptionTable->cellWidget(index, 4);
                        auto* rowAction = actionCell == nullptr
                            ? nullptr
                            : actionCell->findChild<QPushButton*>();
                        if (rowAction != nullptr && rowAction->isEnabled()) {
                            ++remainingCount;
                        }
                    }
                    exceptionCountBadge->setText(QString::number(remainingCount));
                });
    }
    exceptionLayout->insertWidget(1, exceptionTable, 1);
    tableLayout->addWidget(exceptionCard, 1);

    auto* recentOrderCard = createTableCard(
        tr("最新订单"), QString(), tr("查看全部订单  ›"), nullptr, this);
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
    orderTable->setColumnWidth(4, 64);
    const QStringList orderRows[] = {
        {tr("202506010001"), tr("浙A · D12345"), tr("未来科技城充电站"), tr("¥ 45.60"), tr("已支付"), tr("10:28")},
        {tr("202506010002"), tr("浙A · B8C7D"), tr("滨江智慧园充电站"), tr("¥ 32.18"), tr("已支付"), tr("10:21")},
        {tr("202506010003"), tr("浙A · F5566"), tr("城西银泰充电站"), tr("¥ 28.80"), tr("已支付"), tr("10:15")},
        {tr("202506010004"), tr("浙A · E7788"), tr("奥体中心充电站"), tr("¥ 51.20"), tr("进行中"), tr("10:07")},
        {tr("202506010005"), tr("浙A · G9H0I"), tr("萧山机场充电站"), tr("¥ 61.44"), tr("进行中"), tr("09:59")},
    };
    for (int row = 0; row < 5; ++row) {
        setRow(orderTable, row, orderRows[row]);
        auto* state = new QLabel(orderRows[row].at(4), orderTable);
        state->setAlignment(Qt::AlignCenter);
        state->setFixedSize(54, 26);
        state->setStyleSheet(row < 3
                                 ? QStringLiteral("background:#e8f8f0; color:#28a86f; "
                                                  "border-radius:6px; padding:0 7px; font-size:12px;")
                                 : QStringLiteral("background:#eaf3ff; color:#2878f0; "
                                                  "border-radius:6px; padding:0 7px; font-size:12px;"));
        orderTable->setCellWidget(row, 4, createManagementTableCell(state, orderTable));
    }
    orderLayout->insertWidget(1, orderTable, 1);
    tableLayout->addWidget(recentOrderCard, 1);
    layout->addLayout(tableLayout);
}

void DashboardPage::setClientCount(int count)
{
    Q_UNUSED(count);
    // The reference card is a deterministic Mock value until a dedicated dashboard DTO is approved.
}

} // namespace charging::server
