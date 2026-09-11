#include "workflow_management_page.h"
#include "admin_request_gateway.h"
#include "admin_option_loader.h"
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

namespace charging::server {
namespace {
QString statusText(const QString& status)
{
    static const QHash<QString, QString> words{{"WAITING", "等待中"}, {"CALLED", "待确认叫号"},
        {"CONFIRMED", "已确认预约"}, {"LEFT", "已退出"}, {"EXPIRED", "已超时"},
        {"SUBMITTED", "已提交"}, {"ACCEPTED", "已受理"}, {"PROCESSING", "处理中"}, {"RESOLVED", "已恢复"}};
    return words.value(status, status);
}
void setRow(QTableWidget* table, int row, const QStringList& values)
{
    for (int c = 0; c < values.size(); ++c) {
        auto* item = new QTableWidgetItem(values.at(c));
        item->setToolTip(values.at(c));
        table->setItem(row, c, item);
    }
}
QTableWidget* makeTable(QWidget* parent, const QStringList& headers, const QString& name)
{
    auto* table = new QTableWidget(0, headers.size(), parent);
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(46);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setMinimumHeight(180);
    return table;
}
}

WorkflowManagementPage::WorkflowManagementPage(AdminRequestGateway* gateway, QWidget* parent)
    : QWidget(parent), gateway_(gateway)
{
    setObjectName(QStringLiteral("workflowManagementPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);
    auto* hint = new QLabel(tr("FIFO 自动叫号 · 60 秒确认 · 维修操作为模拟处理，不控制真实硬件"), this);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("sectionHintLabel"));
    layout->addWidget(hint);
    auto* filter = new QHBoxLayout;
    station_ = new QComboBox(this);
    station_->setObjectName(QStringLiteral("workflowStationFilter"));
    filter->addWidget(station_, 1);
    auto* refresh = new QPushButton(tr("刷新"), this);
    refresh->setObjectName(QStringLiteral("secondaryButton"));
    filter->addWidget(refresh);
    layout->addLayout(filter);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("workflowTabs"));
    queueTable_ = makeTable(this, {tr("电站"), tr("电桩"), tr("用户"), tr("状态"), tr("队列位置"), tr("确认截止（UTC）")}, "queueTable");
    tabs_->addTab(queueTable_, tr("实时排队"));
    auto* repairPage = new QWidget(this);
    auto* repairLayout = new QVBoxLayout(repairPage);
    repairTable_ = makeTable(repairPage, {tr("编号"), tr("电站 / 电桩"), tr("问题"), tr("状态"), tr("用户"), tr("更新时间（UTC）")}, "repairTable");
    repairLayout->addWidget(repairTable_, 1);
    timeline_ = new QTextEdit(repairPage);
    timeline_->setObjectName(QStringLiteral("repairTimeline"));
    timeline_->setReadOnly(true);
    timeline_->setMinimumHeight(120);
    timeline_->setMaximumHeight(220);
    repairLayout->addWidget(timeline_);
    auto* actions = new QHBoxLayout;
    accept_ = new QPushButton(tr("核实并受理"), repairPage);
    start_ = new QPushButton(tr("开始维修"), repairPage);
    resolve_ = new QPushButton(tr("模拟维修完成"), repairPage);
    retry_ = new QPushButton(tr("重试原操作"), repairPage);
    accept_->setObjectName("acceptRepairButton"); start_->setObjectName("startRepairButton");
    resolve_->setObjectName("resolveRepairButton"); retry_->setObjectName("retryRepairButton");
    for (auto* button : {accept_, start_, resolve_, retry_}) {
        button->setMinimumHeight(38); actions->addWidget(button);
    }
    repairLayout->addLayout(actions);
    tabs_->addTab(repairPage, tr("报障与维修"));
    layout->addWidget(tabs_, 1);
    auto* footer = new QHBoxLayout;
    previous_ = new QPushButton(tr("上一页"), this);
    next_ = new QPushButton(tr("下一页"), this);
    paging_ = new QLabel(this);
    footer->addWidget(paging_); footer->addStretch(); footer->addWidget(previous_); footer->addWidget(next_);
    layout->addLayout(footer);
    message_ = new QLabel(this);
    message_->setObjectName("workflowMessage"); message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    layout->addWidget(message_);
    stationLoader_ = new AdminOptionLoader(gateway, station_, "stations.options", tr("全部电站"), "workflow-stations", this);
    connect(gateway, &AdminRequestGateway::finished, this, &WorkflowManagementPage::receive);
    connect(gateway, &AdminRequestGateway::authenticationChanged, this, [this](bool authenticated) {
        ++authenticationGeneration_;
        listRequest_.clear(); detailRequest_.clear(); mutationRequest_.clear();
        retryPayload_ = {}; retryAction_.clear(); selected_ = {}; repairs_ = {};
        queueTable_->setRowCount(0); repairTable_->setRowCount(0); timeline_->clear(); updateActions();
        if (authenticated) { stationLoader_->reload({}, false); refreshData(); }
    });
    connect(refresh, &QPushButton::clicked, this, &WorkflowManagementPage::refreshData);
    const auto resetPage = [this] {
        page_ = 1; listRequest_.clear(); detailRequest_.clear();
        selected_ = {}; repairs_ = {}; timeline_->clear();
        queueTable_->setRowCount(0); repairTable_->setRowCount(0); updateActions();
        refreshData();
    };
    connect(tabs_, &QTabWidget::currentChanged, this, resetPage);
    connect(station_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this, resetPage](int index) {
            // The option loader restores the previous selected station with
            // blocked signals after “load more”; it is not a new filter.
            if (station_->itemData(index).toString() != QStringLiteral("__load_more__")) resetPage();
        });
    connect(previous_, &QPushButton::clicked, this, [this] { if (page_ > 1) { --page_; refreshData(); } });
    connect(next_, &QPushButton::clicked, this, [this] { if (page_ * 20 < total_) { ++page_; refreshData(); } });
    connect(repairTable_, &QTableWidget::itemSelectionChanged, this, &WorkflowManagementPage::selectReport);
    connect(accept_, &QPushButton::clicked, this, [this] { mutate("repair_reports.accept"); });
    connect(start_, &QPushButton::clicked, this, [this] { mutate("repair_reports.start"); });
    connect(resolve_, &QPushButton::clicked, this, [this] { mutate("repair_reports.resolve"); });
    connect(retry_, &QPushButton::clicked, this, [this] {
        if (!gateway_ || retryPayload_.isEmpty() || !mutationRequest_.isEmpty()) return;
        listRequest_.clear(); detailRequest_.clear();
        mutationRequest_ = gateway_->request(retryAction_, retryPayload_, this, "workflow-mutation"); updateActions();
    });
    refreshTimer_.setInterval(2000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { if (isVisible()) refreshData(); });
    refreshTimer_.start(); updateActions();
    if (gateway->isAuthenticated()) { stationLoader_->reload({}, false); refreshData(); }
}

void WorkflowManagementPage::refreshData()
{
    if (!gateway_ || !gateway_->isAuthenticated() || !listRequest_.isEmpty() || !mutationRequest_.isEmpty()) return;
    stationLoader_->retryIfFailed();
    QJsonObject p{{"page", page_}, {"pageSize", 20}};
    const QString station = station_->currentData().toString();
    bool valid = false;
    if (station.toLongLong(&valid) > 0 && valid) p.insert("stationId", station);
    listRequest_ = gateway_->request(tabs_->currentIndex() == 0 ? "queues.list" : "repair_reports.list", p, this, "workflow-list");
    previous_->setEnabled(false); next_->setEnabled(false);
}

void WorkflowManagementPage::receive(const QString& id, const QJsonObject& response)
{
    const bool ok = response.value("success").toBool();
    const auto data = response.value("data").toObject();
    const auto error = response.value("error").toObject();
    if (id == mutationRequest_ && !id.isEmpty()) {
        mutationRequest_.clear();
        const auto code = error.value("code").toString();
        if (ok || (code != "TIMEOUT" && code != "UNAVAILABLE")) { retryPayload_ = {}; retryAction_.clear(); }
        const auto updated = data.value("item").toObject();
        message_->setText(ok ? tr("报障 #%1 处理成功，状态已同步").arg(updated.value("id").toString())
                            : error.value("message").toString());
        // An operator may select B while A is being saved. A's response must
        // not leave B highlighted with A's detail/actions, even if refresh
        // subsequently fails. Preserve the current selection by identity.
        if (ok && updated.value("id") == selected_.value("id")) {
            selected_ = updated; detailRequest_.clear(); renderDetail();
        }
        updateActions(); refreshData(); return;
    }
    if (id == detailRequest_ && !id.isEmpty()) {
        detailRequest_.clear();
        if (ok && data.value("item").toObject().value("id") == selected_.value("id")) {
            selected_ = data.value("item").toObject(); renderDetail();
        } else if (!ok) message_->setText(error.value("message").toString());
        updateActions(); return;
    }
    if (id != listRequest_ || id.isEmpty()) return;
    listRequest_.clear();
    if (!ok) { message_->setText(error.value("message").toString()); return; }
    const auto items = data.value("items").toArray(); total_ = data.value("total").toInt();
    paging_->setText(tr("第 %1 页 · 共 %2 条").arg(page_).arg(total_));
    previous_->setEnabled(page_ > 1); next_->setEnabled(page_ * 20 < total_);
    if (tabs_->currentIndex() == 0) {
        queueTable_->setRowCount(items.size());
        for (int i = 0; i < items.size(); ++i) {
            const auto row = items.at(i).toObject();
            setRow(queueTable_, i, {row.value("stationName").toString(), row.value("chargerCode").toString(),
                row.value("userPhoneMasked").toString(), statusText(row.value("status").toString()),
                QString::number(row.value("position").toInt()), row.value("callExpiresAt").toString()});
        }
    } else {
        const auto previousId = selected_.value("id");
        repairTable_->blockSignals(true);
        repairs_ = items; repairTable_->setRowCount(items.size());
        int retained = -1;
        for (int i = 0; i < items.size(); ++i) {
            const auto row = items.at(i).toObject();
            setRow(repairTable_, i, {row.value("id").toString(), row.value("stationName").toString() + " / " + row.value("chargerCode").toString(),
                row.value("description").toString(), statusText(row.value("status").toString()),
                row.value("user").toObject().value("phone").toString(), row.value("updatedAt").toString()});
            if (row.value("id") == previousId) retained = i;
        }
        repairTable_->clearSelection();
        if (retained >= 0) repairTable_->selectRow(retained);
        repairTable_->blockSignals(false);
        if (retained >= 0) selectReport();
        else { selected_ = {}; timeline_->clear(); updateActions(); }
    }
}

void WorkflowManagementPage::selectReport()
{
    const int row = repairTable_->currentRow();
    if (row < 0 || row >= repairs_.size() || repairTable_->selectedItems().isEmpty() || !gateway_) {
        selected_ = {}; detailRequest_.clear(); timeline_->clear(); updateActions(); return;
    }
    selected_ = repairs_.at(row).toObject();
    detailRequest_ = gateway_->request("repair_reports.get", {{"id", selected_.value("id")}}, this, "workflow-detail");
    renderDetail(); updateActions();
}
void WorkflowManagementPage::renderDetail()
{
    QString text = tr("报障 #%1 · %2\n%3\n\n").arg(selected_.value("id").toString(),
        selected_.value("chargerCode").toString(), selected_.value("description").toString());
    for (const auto event : selected_.value("timeline").toArray()) {
        const auto entry = event.toObject();
        text += QStringLiteral("● %1  %2\n   %3\n").arg(statusText(entry.value("status").toString()),
            entry.value("createdAt").toString(), entry.value("note").toString());
    }
    timeline_->setPlainText(text);
}
void WorkflowManagementPage::updateActions()
{
    const int row = repairTable_->currentRow();
    const bool selectionMatches = row >= 0 && row < repairs_.size() && !repairTable_->selectedItems().isEmpty()
        && repairs_.at(row).toObject().value("id") == selected_.value("id");
    const bool ready = gateway_ && gateway_->isAuthenticated() && mutationRequest_.isEmpty()
        && retryPayload_.isEmpty() && detailRequest_.isEmpty() && selectionMatches;
    const auto status = selected_.value("status").toString();
    accept_->setEnabled(ready && status == "SUBMITTED");
    start_->setEnabled(ready && status == "ACCEPTED");
    resolve_->setEnabled(ready && status == "PROCESSING");
    retry_->setVisible(!retryPayload_.isEmpty()); retry_->setEnabled(mutationRequest_.isEmpty());
}
void WorkflowManagementPage::mutate(const QString& action)
{
    if (!gateway_ || selected_.isEmpty() || !mutationRequest_.isEmpty() || !retryPayload_.isEmpty()) return;
    const QJsonObject snapshot = selected_;
    const quint64 generation = authenticationGeneration_;
    bool accepted = false;
    const QString note = QInputDialog::getMultiLineText(this, tr("维修处理说明"),
        tr("请输入核实或处理说明（1–200 字）；此操作会更新电桩和报障进度。"), {}, &accepted).trimmed();
    if (!accepted) return;
    if (!gateway_ || !gateway_->isAuthenticated() || generation != authenticationGeneration_)
        return;
    if (note.isEmpty() || note.size() > 200) { message_->setText(tr("处理说明须为 1–200 字")); return; }
    retryAction_ = action;
    retryPayload_ = {{"id", snapshot.value("id")}, {"expectedUpdatedAt", snapshot.value("updatedAt")},
        {"operationId", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"note", note}};
    listRequest_.clear(); detailRequest_.clear();
    mutationRequest_ = gateway_->request(action, retryPayload_, this, "workflow-mutation"); updateActions();
}
}
