#include "charging/client/profile_charging/wallet_page.h"

#include "charging/client/profile_charging/client_errors.h"
#include "charging/client/profile_charging/presentation_format.h"
#include "charging/client/widgets/action_button.h"
#include "charging/client/widgets/card.h"
#include "charging/client/widgets/loading_overlay.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"
#include "charging/client/widgets/toast.h"
#include "charging/common/protocol/protocol.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace charging::client {

WalletPage::WalletPage(WalletService* service, QWidget* parent) : QWidget(parent), service_(service)
{
    buildUi();

    connect(service_, &WalletService::profileLoaded, this, &WalletPage::onProfileLoaded);
    connect(service_, &WalletService::rechargeRecordsLoaded, this, &WalletPage::onRecordsLoaded);
    connect(service_, &WalletService::operationFailed, this, &WalletPage::onOperationFailed);
}

void WalletPage::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(14);

    auto* headerRow = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("钱包"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    auto* ordersButton = new ActionButton(ActionButton::Variant::Ghost, tr("我的订单 ›"), this);
    connect(ordersButton, &ActionButton::clicked, this, &WalletPage::ordersRequested);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    headerRow->addWidget(ordersButton);
    rootLayout->addLayout(headerRow);

    balanceCard_ = new Card(this);
    auto* balanceLayout = balanceCard_->bodyLayout();

    nicknameLabel_ = new QLabel(tr("你好"), balanceCard_);
    nicknameLabel_->setProperty("role", QStringLiteral("subtitle"));
    phoneLabel_ = new QLabel(QString(), balanceCard_);
    phoneLabel_->setProperty("role", QStringLiteral("caption"));
    balanceLayout->addWidget(nicknameLabel_);
    balanceLayout->addWidget(phoneLabel_);

    auto* balanceCaption = new QLabel(tr("当前余额（元）"), balanceCard_);
    balanceCaption->setProperty("role", QStringLiteral("caption"));
    balanceValueLabel_ = new QLabel(QStringLiteral("¥ --"), balanceCard_);
    balanceValueLabel_->setProperty("role", QStringLiteral("balance"));
    balanceLayout->addWidget(balanceCaption);
    balanceLayout->addWidget(balanceValueLabel_);

    rechargeButton_ = new ActionButton(ActionButton::Variant::Primary, tr("充 值"), balanceCard_);
    rechargeButton_->setMinimumHeight(46);
    connect(rechargeButton_, &ActionButton::clicked, this, &WalletPage::rechargeRequested);
    balanceLayout->addWidget(rechargeButton_);

    profileNotice_ = new NoticePanel(QStringLiteral("⚠"), tr("余额获取失败"), QString(),
                                     tr("重试"), balanceCard_);
    profileNotice_->setVisible(false);
    connect(profileNotice_, &NoticePanel::actionTriggered, this, &WalletPage::refresh);
    balanceLayout->addWidget(profileNotice_);

    rootLayout->addWidget(balanceCard_);

    auto* recordsTitle = new QLabel(tr("充值记录"), this);
    recordsTitle->setProperty("role", QStringLiteral("sectionTitle"));
    rootLayout->addWidget(recordsTitle);

    recordsScroll_ = new QScrollArea(this);
    recordsScroll_->setObjectName(QStringLiteral("uiRecordsScroll"));
    recordsScroll_->setWidgetResizable(true);
    recordsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* recordsContainer = new QWidget(recordsScroll_);
    recordsListLayout_ = new QVBoxLayout(recordsContainer);
    recordsListLayout_->setContentsMargins(0, 0, 0, 0);
    recordsListLayout_->setSpacing(8);
    recordsListLayout_->addStretch();
    recordsScroll_->setWidget(recordsContainer);
    rootLayout->addWidget(recordsScroll_, 1);

    recordsNotice_ = new NoticePanel(QStringLiteral("—"), tr("暂无充值记录"), QString(),
                                     QString(), this);
    recordsNotice_->setVisible(false);
    connect(recordsNotice_, &NoticePanel::actionTriggered, this, &WalletPage::refresh);
    rootLayout->addWidget(recordsNotice_);

    loadMoreButton_ = new ActionButton(ActionButton::Variant::Secondary, tr("加载更多"), this);
    loadMoreButton_->setVisible(false);
    connect(loadMoreButton_, &ActionButton::clicked, this, [this]() {
        if (!hasMoreRecords_ || service_->isFetchingRecords()) {
            return;
        }
        loadingPage_ = currentPage_ + 1;
        beginBusy();
        service_->fetchRechargeRecords(loadingPage_);
    });
    rootLayout->addWidget(loadMoreButton_);

    overlay_ = new LoadingOverlay(this);
    overlay_->setVisible(false);
}

void WalletPage::refresh()
{
    // One busy unit per in-flight request so the overlay only clears after
    // both the profile and the first records page have answered.
    loadingPage_ = 1;
    beginBusy();
    service_->fetchProfile();
    beginBusy();
    service_->fetchRechargeRecords(1);
}

void WalletPage::beginBusy()
{
    ++busyCount_;
    if (busyCount_ == 1) {
        overlay_->showFor();
    }
}

void WalletPage::endBusy()
{
    busyCount_ = busyCount_ > 0 ? busyCount_ - 1 : 0;
    if (busyCount_ == 0) {
        overlay_->hideFor();
    }
}

void WalletPage::onProfileLoaded(const charging::model::User& user)
{
    endBusy();

    nicknameLabel_->setText(tr("你好，%1").arg(user.nickname));
    phoneLabel_->setText(user.phone);
    balanceValueLabel_->setText(QStringLiteral("¥ %1").arg(formatCentsAsYuan(user.balanceCents)));
    profileNotice_->setVisible(false);
}

void WalletPage::onRecordsLoaded(const QVector<charging::model::RechargeRecord>& records,
                                 int total, bool hasMore)
{
    endBusy();

    const bool firstPage = loadingPage_ <= 1;
    if (firstPage) {
        clearRecordRows();
        shownRecords_.clear();
    }
    for (const charging::model::RechargeRecord& record : records) {
        shownRecords_.append(record);
        // Insert before the trailing stretch item.
        recordsListLayout_->insertWidget(recordsListLayout_->count() - 1,
                                         buildRecordRow(record));
    }
    currentPage_ = loadingPage_;
    hasMoreRecords_ = hasMore;

    if (shownRecords_.isEmpty()) {
        showRecordsNotice(QStringLiteral("—"), tr("暂无充值记录"),
                          tr("完成第一笔充值后，记录会显示在这里"), QString());
    } else {
        hideRecordsNotice();
    }
    loadMoreButton_->setVisible(hasMore && !shownRecords_.isEmpty());
    Q_UNUSED(total);
}

void WalletPage::onOperationFailed(const QString& type,
                                   const charging::protocol::ProtocolError& error)
{
    endBusy();
    Toast::show(this, displayMessageForError(error), StatusTag::Tone::Danger);

    const QString profileType =
        QString::fromLatin1(charging::protocol::request_type::kGetUserInfo);
    if (type == profileType) {
        balanceValueLabel_->setText(QStringLiteral("¥ --"));
        profileNotice_->setContent(QStringLiteral("⚠"), tr("余额获取失败"),
                                   displayMessageForError(error), tr("重试"));
        profileNotice_->setVisible(true);
        return;
    }

    const QString recordsType =
        QString::fromLatin1(charging::protocol::request_type::kGetRechargeRecords);
    if (type == recordsType && shownRecords_.isEmpty()) {
        showRecordsNotice(QStringLiteral("⚠"), tr("充值记录加载失败"),
                          displayMessageForError(error), tr("重试"));
    }
    // A failed page > 1 keeps the already-rendered rows; the user can retry
    // "加载更多" without losing context.
}

QWidget* WalletPage::buildRecordRow(const charging::model::RechargeRecord& record)
{
    // Plain styled frame (not Card) so the horizontal row owns the only layout.
    auto* row = new QFrame(this);
    row->setObjectName(QStringLiteral("uiCard"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(16, 12, 16, 12);
    rowLayout->setSpacing(10);

    auto* leftLayout = new QVBoxLayout();
    leftLayout->setSpacing(2);
    auto* transactionLabel = new QLabel(record.transactionNo, row);
    transactionLabel->setProperty("role", QStringLiteral("caption"));
    auto* timeLabel = new QLabel(formatDateTimeLocal(record.createdAtUtc), row);
    timeLabel->setProperty("role", QStringLiteral("secondary"));
    leftLayout->addWidget(transactionLabel);
    leftLayout->addWidget(timeLabel);

    auto* rightLayout = new QVBoxLayout();
    rightLayout->setSpacing(2);
    rightLayout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    const QString amountText = QStringLiteral("+%1").arg(formatCentsAsYuan(record.amountCents));
    auto* amountLabel = new QLabel(amountText, row);
    amountLabel->setProperty("role", QStringLiteral("amountPositive"));
    amountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* metaRow = new QHBoxLayout();
    metaRow->setSpacing(6);
    const bool success = record.status == charging::model::RechargeStatus::Success;
    auto* statusTag = new StatusTag(success ? tr("成功") : tr("失败"),
                                    success ? StatusTag::Tone::Success : StatusTag::Tone::Danger,
                                    row);
    auto* balanceAfterLabel = new QLabel(
        tr("余额 ¥%1").arg(formatCentsAsYuan(record.balanceAfterCents)), row);
    balanceAfterLabel->setProperty("role", QStringLiteral("caption"));
    metaRow->addWidget(balanceAfterLabel);
    metaRow->addWidget(statusTag);

    rightLayout->addWidget(amountLabel);
    rightLayout->addLayout(metaRow);

    rowLayout->addLayout(leftLayout);
    rowLayout->addStretch();
    rowLayout->addLayout(rightLayout);
    return row;
}

void WalletPage::clearRecordRows()
{
    // Remove every widget except the trailing stretch item.
    while (recordsListLayout_->count() > 1) {
        QLayoutItem* item = recordsListLayout_->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
}

void WalletPage::showRecordsNotice(const QString& glyph, const QString& title,
                                   const QString& description, const QString& actionText)
{
    recordsScroll_->setVisible(false);
    recordsNotice_->setContent(glyph, title, description, actionText);
    recordsNotice_->setVisible(true);
}

void WalletPage::hideRecordsNotice()
{
    recordsNotice_->setVisible(false);
    recordsScroll_->setVisible(true);
}

} // namespace charging::client
