#pragma once

#include "charging/common/model/models.h"

#include <QWidget>

class QLabel;

namespace charging::client {
class NoticePanel;
class StatusTag;
}

namespace charging::client::pages::station {

// 站点详情页（占位路由，任务 #7 仅打通跳转；详情业务属任务 #12）。
//
// 展示入口卡片带来的站点基础字段与“返回”；后续任务 #12 在此接入
// 桩位列表、价格明细与预约入口（任务 #17），本次不实现。
class StationDetailPage final : public QWidget
{
    Q_OBJECT

public:
    explicit StationDetailPage(QWidget* parent = nullptr);

    void openStation(const charging::model::Station& station, int distanceMeters);

signals:
    void backRequested();

private:
    QLabel* nameLabel_ = nullptr;
    QLabel* addressLabel_ = nullptr;
    QLabel* priceLabel_ = nullptr;
    QLabel* distanceLabel_ = nullptr;
    charging::client::StatusTag* statusTag_ = nullptr;
    NoticePanel* placeholderNotice_ = nullptr;
};

} // namespace charging::client::pages::station
