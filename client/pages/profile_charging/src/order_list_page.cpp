#include "charging/client/profile_charging/order_list_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/order_status_display.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/clickable_card.h"
#include "charging/client/widgets/loading_overlay.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace charging::client {

namespace {

constexpr int kFilterCount = 4;

// 行内展示时间：与 buildOrderRow 一致，开始时间优先、回退创建时间。
// 契约 v1 §3 冻结 GET_ORDERS 为 createdAt DESC, id DESC，展示时间统一取
// createdAt：与分页顺序、月度分组同一把尺，避免跨页拼接后再次乱序。
QDateTime orderDisplayTime(const charging::client::OrderSummary& summary)
{
    return summary.order.createdAtUtc;
}

// 月度分组的月份键（本地时区 yyyy-MM）；无效时间归入空键组。
QString monthKeyOf(const QDateTime& utcValue)
{
    const QDateTime local = utcValue.toLocalTime();
    return local.isValid() ? local.toString(QStringLiteral("yyyy-MM")) : QString();
}

} // namespace

OrderListPage::OrderListPage(OrderService* service, QWidget* parent)
    : QWidget(parent), service_(service)
{
    buildUi();

    connect(service_, &OrderService::ordersLoaded, this, &OrderListPage::onOrdersLoaded);
    connect(service_, &OrderService::operationFailed, this, &OrderListPage::onOperationFailed);
}

void OrderListPage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(14);

    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("我的订单"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    backButton_ = new ActionButton(ActionButton::Variant::Ghost, tr("返回"), this);
    connect(backButton_, &ActionButton::clicked, this, &OrderListPage::backRequested);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    headerRow->addWidget(backButton_);
    rootLayout->addLayout(headerRow);

    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(10);
    const QString filterNames[kFilterCount] = {tr("全部"), tr("充电中"), tr("待支付"),
                                               tr("已完成")};
    const OrderService::Filter filterValues[kFilterCount] = {
        OrderService::Filter::All, OrderService::Filter::Charging,
        OrderService::Filter::WaitingPayment, OrderService::Filter::Completed};
    for (int index = 0; index < kFilterCount; ++index) {
        auto* chip = new ActionButton(ActionButton::Variant::Chip, filterNames[index], this);
        chip->setMinimumHeight(40);
        filterChips_.append(chip);
        filterValues_.append(filterValues[index]);
        connect(chip, &ActionButton::toggled, this, [this, index](bool checked) {
            if (applyingFilter_ || !checked) {
                return;
            }
            applyingFilter_ = true;
            for (int other = 0; other < filterChips_.size(); ++other) {
                if (other != index) {
                    filterChips_.at(other)->setChecked(other == index);
                }
            }
            applyingFilter_ = false;
            applyFilter(filterValues_.at(index));
        });
        filterRow->addWidget(chip);
    }
    applyingFilter_ = true;
    filterChips_.first()->setChecked(true);
    applyingFilter_ = false;
    rootLayout->addLayout(filterRow);

    listScroll_ = new QScrollArea(this);
    listScroll_->setObjectName(QStringLiteral("uiRecordsScroll"));
    listScroll_->setWidgetResizable(true);
    listScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* listContainer = new QWidget(listScroll_);
    listLayout_ = new QVBoxLayout(listContainer);
    listLayout_->setContentsMargins(0, 0, 0, 0);
    listLayout_->setSpacing(8);
    listLayout_->addStretch();
    listScroll_->setWidget(listContainer);

    listNotice_ = new NoticePanel(QStringLiteral("—"), tr("暂无订单"), QString(), QString(), this);
    connect(listNotice_, &NoticePanel::actionTriggered, this, &OrderListPage::refresh);
    // 列表与空态/错误态放同一 QStackedWidget：此前用 setVisible 互斥，空态时
    // 滚动区（唯一有拉伸因子的项）被隐藏，提示缩在顶部一小条、下方大片空白。
    listStack_ = new QStackedWidget(this);
    listStack_->setObjectName(QStringLiteral("uiOrderListStack"));
    listStack_->addWidget(listScroll_); // 0：正常列表
    listStack_->addWidget(listNotice_); // 1：空态 / 错误态
    rootLayout->addWidget(listStack_, 1);

    loadMoreButton_ = new ActionButton(ActionButton::Variant::Secondary, tr("加载更多"), this);
    loadMoreButton_->setVisible(false);
    connect(loadMoreButton_, &ActionButton::clicked, this, [this]() {
        if (!hasMoreOrders_ || service_->isFetchingOrders()) {
            return;
        }
        loadingPage_ = currentPage_ + 1;
        beginBusy();
        service_->fetchOrders(currentFilter_, loadingPage_);
    });
    rootLayout->addWidget(loadMoreButton_);

    overlay_ = new LoadingOverlay(this);
    overlay_->setVisible(false);
}

void OrderListPage::setEmbedded(bool embedded)
{
    // 壳层 Tab 根页不需要返回按钮；全局顶部导航负责路由页返回。
    backButton_->setVisible(!embedded);
}

void OrderListPage::refresh()
{
    if (service_->isFetchingOrders()) {
        return;
    }
    loadingPage_ = 1;
    beginBusy();
    service_->fetchOrders(currentFilter_, loadingPage_);
}

void OrderListPage::showFilter(OrderService::Filter filter)
{
    // Entry from elsewhere (profile hub badge cells): sync the chip row the
    // same way a chip click would, then run the shared filter path.
    applyingFilter_ = true;
    for (int index = 0; index < filterChips_.size(); ++index) {
        filterChips_.at(index)->setChecked(filterValues_.at(index) == filter);
    }
    applyingFilter_ = false;
    applyFilter(filter);
}

void OrderListPage::applyFilter(OrderService::Filter filter)
{
    if (service_->isFetchingOrders()) {
        // The service ignores requests while one is in flight; keep the chips
        // consistent with the filter actually being shown.
        applyingFilter_ = true;
        for (int index = 0; index < filterChips_.size(); ++index) {
            filterChips_.at(index)->setChecked(filterValues_.at(index) == currentFilter_);
        }
        applyingFilter_ = false;
        return;
    }
    currentFilter_ = filter;
    refresh();
}

void OrderListPage::beginBusy()
{
    ++busyCount_;
    if (busyCount_ == 1) {
        overlay_->showFor();
    }
}

void OrderListPage::endBusy()
{
    busyCount_ = busyCount_ > 0 ? busyCount_ - 1 : 0;
    if (busyCount_ == 0) {
        overlay_->hideFor();
    }
}

void OrderListPage::onOrdersLoaded(const QVector<charging::client::OrderSummary>& orders,
                                   int total, bool hasMore)
{
    endBusy();

    const bool firstPage = loadingPage_ <= 1;
    if (firstPage) {
        shownOrders_.clear();
    }
    for (const charging::client::OrderSummary& summary : orders) {
        shownOrders_.append(summary);
    }
    // 契约 v1 §3 已冻结服务端排序（createdAt DESC, id DESC），跨页拼接保持
    // 服务端原序即可全局有序；客户端不得再排序（否则分页窗口错位的假象会
    // 掩盖真实数据问题，月度分组表头也会随之乱序）。
    rebuildMonthGroups();
    currentPage_ = loadingPage_;
    hasMoreOrders_ = hasMore;

    if (shownOrders_.isEmpty()) {
        showListNotice(QStringLiteral("—"), tr("暂无订单"),
                       tr("完成充电后，订单会显示在这里"), QString());
    } else {
        hideListNotice();
    }
    loadMoreButton_->setVisible(hasMore && !shownOrders_.isEmpty());
    Q_UNUSED(total);
}

void OrderListPage::onOperationFailed(const QString& type,
                                      const charging::protocol::ProtocolError& error)
{
    endBusy();
    Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);

    const QString ordersType =
        QString::fromLatin1(charging::protocol::request_type::kGetOrders);
    if (type == ordersType && shownOrders_.isEmpty()) {
        showListNotice(QStringLiteral("⚠"), tr("订单加载失败"), displayMessageForError(error),
                       tr("重试"));
    }
    // A failed page > 1 keeps the already-rendered rows; "加载更多" retries.
}

QWidget* OrderListPage::buildOrderRow(const charging::client::OrderSummary& summary)
{
    auto* row = new ClickableCard(this);
    auto* rowLayout = row->bodyLayout();
    rowLayout->setContentsMargins(16, 12, 16, 12);
    rowLayout->setSpacing(4);

    auto* topRow = new QHBoxLayout();
    auto* stationLabel = new QLabel(summary.stationName.isEmpty() ? tr("未知充电站")
                                                                  : summary.stationName,
                                    row);
    stationLabel->setProperty("role", QStringLiteral("sectionTitle"));
    const OrderStatusDisplay statusDisplay = orderStatusDisplay(summary.order.status);
    auto* statusTag = new StatusTag(statusDisplay.text, statusDisplay.tone, row);
    topRow->addWidget(stationLabel);
    topRow->addStretch();
    topRow->addWidget(statusTag);

    const QDateTime displayTime = orderDisplayTime(summary);
    auto* metaLabel = new QLabel(
        tr("桩号 %1 · %2").arg(summary.chargerCode.isEmpty() ? QStringLiteral("--")
                                                             : summary.chargerCode,
                               formatDateTimeLocal(displayTime)),
        row);
    metaLabel->setProperty("role", QStringLiteral("secondary"));

    auto* bottomRow = new QHBoxLayout();
    auto* orderNoLabel = new QLabel(summary.order.orderNo, row);
    orderNoLabel->setProperty("role", QStringLiteral("caption"));
    auto* amountLabel =
        new QLabel(QStringLiteral("¥%1").arg(formatCentsAsYuan(summary.order.amountCents)), row);
    amountLabel->setProperty("role", QStringLiteral("amountStrong"));
    bottomRow->addWidget(orderNoLabel);
    bottomRow->addStretch();
    bottomRow->addWidget(amountLabel);

    rowLayout->addLayout(topRow);
    rowLayout->addWidget(metaLabel);
    rowLayout->addLayout(bottomRow);

    const charging::client::OrderSummary captured = summary;
    connect(row, &ClickableCard::clicked, this, [this, captured]() {
        emit orderOpened(captured);
    });
    return row;
}

void OrderListPage::clearOrderRows()
{
    // Remove every widget except the trailing stretch item.
    while (listLayout_->count() > 1) {
        QLayoutItem* item = listLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
}

void OrderListPage::rebuildMonthGroups()
{
    clearOrderRows();

    // shownOrders_ 已按时间倒序，同月订单连续：月变化时插一次表头。表头汇总
    // 只是把该月可见行相加，服务端仍是唯一事实来源（TODO(contract) 无月度
    // 汇总接口，客户端不加新协议）。
    auto addBeforeStretch = [this](QWidget* widget) {
        listLayout_->insertWidget(listLayout_->count() - 1, widget);
    };
    auto summaryText = [this](int count, qint64 cents, qint64 wh) {
        return tr("%1 单 · ¥%2 · %3 kWh").arg(count).arg(formatCentsAsYuan(cents),
                                                          formatEnergyWhAsKwh(wh));
    };

    QString currentMonth;
    QLabel* monthSummaryLabel = nullptr;
    int monthCount = 0;
    qint64 monthAmountCents = 0;
    qint64 monthEnergyWh = 0;

    for (const charging::client::OrderSummary& summary : shownOrders_) {
        const QString month = monthKeyOf(orderDisplayTime(summary));
        if (month != currentMonth) {
            if (monthSummaryLabel != nullptr) {
                monthSummaryLabel->setText(summaryText(monthCount, monthAmountCents,
                                                       monthEnergyWh));
            }
            currentMonth = month;
            monthCount = 0;
            monthAmountCents = 0;
            monthEnergyWh = 0;
            addBeforeStretch(buildMonthHeader(month, &monthSummaryLabel));
        }
        ++monthCount;
        monthAmountCents += summary.order.amountCents;
        monthEnergyWh += summary.order.energyWh;
        addBeforeStretch(buildOrderRow(summary));
    }
    if (monthSummaryLabel != nullptr) {
        monthSummaryLabel->setText(summaryText(monthCount, monthAmountCents, monthEnergyWh));
    }
}

QWidget* OrderListPage::buildMonthHeader(const QString& monthKey, QLabel** summaryOut)
{
    const QDate date = QDate::fromString(monthKey, QStringLiteral("yyyy-MM"));
    const QString title = date.isValid()
                              ? QStringLiteral("%1年%2月").arg(date.year()).arg(date.month())
                              : tr("更早"); // 时间无效的行兜底归组，正常情况下不出现。

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("uiMonthHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(4, 8, 4, 0);
    headerLayout->setSpacing(8);
    auto* titleLabel = new QLabel(title, header);
    titleLabel->setObjectName(QStringLiteral("uiMonthTitle"));
    auto* summaryLabel = new QLabel(header); // 文本在整月行汇总完后回填。
    summaryLabel->setObjectName(QStringLiteral("uiMonthSummary"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(summaryLabel);
    *summaryOut = summaryLabel;
    return header;
}

void OrderListPage::showListNotice(const QString& glyph, const QString& title,
                                   const QString& description, const QString& actionText)
{
    listNotice_->setContent(glyph, title, description, actionText);
    listStack_->setCurrentWidget(listNotice_);
}

void OrderListPage::hideListNotice()
{
    listStack_->setCurrentWidget(listScroll_);
}

} // namespace charging::client
