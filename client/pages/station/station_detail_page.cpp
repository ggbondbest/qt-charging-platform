#include "pages/station/station_detail_page.h"

#include "charging/client/widgets/card.h"
#include "charging/client/widgets/notice_panel.h"
#include "charging/client/widgets/status_tag.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace charging::client::pages::station {

namespace {

QString formatDistance(int meters)
{
    if (meters < 0) {
        return QStringLiteral("--");
    }
    if (meters < 1000) {
        return QStringLiteral("%1 m").arg(meters);
    }
    return QStringLiteral("%1 km").arg(meters / 1000.0, 0, 'f', 1);
}

} // namespace

StationDetailPage::StationDetailPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("stationDetailPage"));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 12, 16, 12);
    rootLayout->setSpacing(10);

    // 返回 + 页面标题。
    auto* headerRow = new QHBoxLayout();
    auto* backButton = new QPushButton(tr("‹ 返回"), this);
    backButton->setObjectName(QStringLiteral("detailBackButton"));
    backButton->setCursor(Qt::PointingHandCursor);
    connect(backButton, &QPushButton::clicked, this, [this]() { emit backRequested(); });
    auto* titleLabel = new QLabel(tr("站点详情"), this);
    titleLabel->setProperty("role", QStringLiteral("pageTitle"));
    headerRow->addWidget(backButton);
    headerRow->addSpacing(8);
    headerRow->addWidget(titleLabel);
    headerRow->addStretch();
    rootLayout->addLayout(headerRow);

    // 入口信息卡片：仅展示列表页已透传的基础字段。
    auto* card = new Card(this);
    card->setProperty("isDetailCard", true);
    auto* body = card->bodyLayout();

    auto* titleRow = new QHBoxLayout();
    nameLabel_ = new QLabel(card);
    nameLabel_->setObjectName(QStringLiteral("detailNameLabel"));
    nameLabel_->setProperty("role", QStringLiteral("sectionTitle"));
    statusTag_ = new StatusTag(QString(), StatusTag::Tone::Success, card);
    statusTag_->setObjectName(QStringLiteral("detailStatusTag"));
    titleRow->addWidget(nameLabel_);
    titleRow->addStretch();
    titleRow->addWidget(statusTag_);
    body->addLayout(titleRow);

    addressLabel_ = new QLabel(card);
    addressLabel_->setObjectName(QStringLiteral("detailAddressLabel"));
    addressLabel_->setProperty("role", QStringLiteral("secondary"));
    body->addWidget(addressLabel_);

    auto* detailRow = new QHBoxLayout();
    priceLabel_ = new QLabel(card);
    priceLabel_->setObjectName(QStringLiteral("detailPriceLabel"));
    priceLabel_->setProperty("role", QStringLiteral("amountStrong"));
    distanceLabel_ = new QLabel(card);
    distanceLabel_->setObjectName(QStringLiteral("detailDistanceLabel"));
    distanceLabel_->setProperty("role", QStringLiteral("secondary"));
    detailRow->addWidget(priceLabel_);
    detailRow->addStretch();
    detailRow->addWidget(distanceLabel_);
    body->addLayout(detailRow);
    rootLayout->addWidget(card);

    // 详情业务占位说明（任务 #12/#17 接入，本次不实现）。
    placeholderNotice_ = new NoticePanel(QStringLiteral("🛈"), tr("详情功能建设中"),
                                         tr("实时桩位列表、价格明细与预约入口将在任务 #12（站点详情）"
                                            "与任务 #17（预约业务）接入，本页面当前仅承接路由跳转。"),
                                         QString(), this);
    rootLayout->addWidget(placeholderNotice_, 1);
    rootLayout->addStretch();
}

void StationDetailPage::openStation(const charging::model::Station& station, int distanceMeters)
{
    nameLabel_->setText(station.name);
    addressLabel_->setText(station.address);
    priceLabel_->setText(QStringLiteral("¥%1/度")
                             .arg(QString::number(station.priceCentsPerKwh / 100.0, 'f', 2)));
    distanceLabel_->setText(tr("距离 %1").arg(formatDistance(distanceMeters)));
    statusTag_->setText(station.availableChargers > 0
                            ? tr("空闲 %1/%2")
                                  .arg(station.availableChargers)
                                  .arg(station.totalChargers)
                            : tr("桩位已满"));
    statusTag_->setTone(station.availableChargers > 0 ? StatusTag::Tone::Success
                                                      : StatusTag::Tone::Danger);
}

} // namespace charging::client::pages::station
