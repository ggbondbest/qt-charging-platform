#include "admin_option_loader.h"
#include "admin_request_gateway.h"
#include "management_page_widgets.h"

#include <QComboBox>
#include <QJsonArray>
#include <QLineEdit>
#include <QSignalBlocker>

namespace charging::server {
namespace { const QString moreValue = QStringLiteral("__load_more__"); }

AdminOptionLoader::AdminOptionLoader(AdminRequestGateway* gateway, QComboBox* combo,
                                     const QString& action, const QString& placeholder,
                                     const QString& scope, QObject* parent)
    : QObject(parent), gateway_(gateway), combo_(combo), action_(action),
      placeholder_(placeholder), scope_(scope)
{
    combo_->setEditable(true);
    combo_->setInsertPolicy(QComboBox::NoInsert);
    combo_->setCompleter(nullptr); // Filtering is performed by the server.
    combo_->lineEdit()->setMaxLength(64);
    applyManagementLightPalette(combo_->lineEdit());
    combo_->setToolTip(tr("输入关键词搜索；选择“加载更多”取得下一页"));
    debounce_.setSingleShot(true);
    debounce_.setInterval(300);
    connect(&debounce_, &QTimer::timeout, this, [this] { requestPage(1); });
    connect(combo_->lineEdit(), &QLineEdit::textEdited, this, [this](const QString& text) {
        keyword_ = text.trimmed();
        selectedId_.clear();
        // Typed search text is not a selected database ID.
        const QSignalBlocker blocker(combo_);
        combo_->setCurrentIndex(-1);
        combo_->setEditText(text);
        requestId_.clear(); // Discard a response for the preceding keyword.
        requestFailed_ = false; // A new search supersedes the failed older query.
        debounce_.start();
    });
    connect(combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        const QString value = combo_->itemData(index).toString();
        if (value != moreValue) selectedId_ = value;
    });
    connect(combo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (combo_->itemData(index).toString() == moreValue) {
            const QSignalBlocker blocker(combo_);
            const int previous = combo_->findData(selectedId_);
            combo_->setCurrentIndex(previous >= 0 ? previous : 0);
            if (nextPage_ > 0) requestPage(nextPage_);
        }
    });
    connect(gateway, &AdminRequestGateway::finished, this,
            [this](const QString& id, const QJsonObject& response) {
        if (id == requestId_) receive(response);
    });
}

void AdminOptionLoader::reload(const QJsonObject& filters, bool keepSelection)
{
    debounce_.stop();
    requestId_.clear();
    requestFailed_ = false;
    filters_ = filters;
    keyword_.clear();
    if (!keepSelection) {
        selectedId_.clear();
        const QSignalBlocker blocker(combo_);
        combo_->clear(); combo_->addItem(placeholder_, QString());
    }
    requestPage(1);
}

void AdminOptionLoader::requestPage(int page)
{
    if (!gateway_ || !gateway_->isAuthenticated()) return;
    requestedPage_ = page;
    requestFailed_ = false;
    auto query = filters_;
    query.insert(QStringLiteral("page"), page);
    query.insert(QStringLiteral("pageSize"), 50);
    if (!keyword_.isEmpty()) query.insert(QStringLiteral("keyword"), keyword_);
    requestId_ = gateway_->request(action_, query, this, scope_);
    if (requestId_.isEmpty()) requestFailed_ = true;
}

void AdminOptionLoader::retryIfFailed()
{
    if (requestFailed_) requestPage(requestedPage_);
}

void AdminOptionLoader::receive(const QJsonObject& response)
{
    if (!response.value(QStringLiteral("success")).toBool()) {
        requestFailed_ = true;
        combo_->setToolTip(tr("选项加载失败，重新输入关键词或刷新可重试"));
        return; // Keep the previously confirmed options on refresh failure.
    }
    requestFailed_ = false;
    const auto data = response.value(QStringLiteral("data")).toObject();
    const int page = data.value(QStringLiteral("page")).toInt(1);
    const auto rows = data.value(QStringLiteral("items")).toArray();
    const QString selectedId = combo_->currentData().toString();
    selectedId_ = selectedId;
    const QString selectedText = combo_->currentText();
    const QSignalBlocker blocker(combo_);
    if (page == 1) { combo_->clear(); combo_->addItem(placeholder_, QString()); }
    const int oldMore = combo_->findData(moreValue);
    if (oldMore >= 0) combo_->removeItem(oldMore);
    for (const auto& row : rows) {
        const auto item = row.toObject();
        const QString id = item.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || combo_->findData(id) >= 0) continue;
        QString label = item.value(QStringLiteral("name")).toString();
        if (action_ == QStringLiteral("chargers.options"))
            label = item.value(QStringLiteral("code")).toString() + tr("（%1）").arg(item.value(QStringLiteral("stationName")).toString());
        combo_->addItem(label, id);
    }
    nextPage_ = page * data.value(QStringLiteral("pageSize")).toInt(50)
                        < data.value(QStringLiteral("total")).toInt() && !rows.isEmpty() ? page + 1 : 0;
    if (!selectedId.isEmpty() && selectedId != moreValue && combo_->findData(selectedId) < 0)
        combo_->addItem(selectedText, selectedId);
    if (nextPage_) combo_->addItem(tr("加载更多…"), moreValue);
    const int selected = combo_->findData(selectedId);
    combo_->setCurrentIndex(selected >= 0 ? selected : 0);
    if (!keyword_.isEmpty() && selectedId.isEmpty()) combo_->setEditText(keyword_);
    combo_->setToolTip(tr("输入关键词搜索；选择“加载更多”取得下一页"));
}
} // namespace charging::server
