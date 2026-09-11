#pragma once

#include "charging/client/profile_charging/charging_service.h"
#include "charging/common/protocol/protocol.h"

#include <QWidget>

class QLabel;

namespace charging::client {

class ActionBar;
class ActionButton;
class Card;
class ChargingPulse;
class LoadingOverlay;
class NoticePanel;
class StatusTag;

// Live charging session page. Every number on screen is whatever the last
// GET_CHARGING_STATUS returned (the service polls; this page only renders).
// The stop button asks the server to end the session; settlement navigates
// only after the server confirms the transition.
class ChargingPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ChargingPage(ChargingService* service, QWidget* parent = nullptr);

    // Entry point: start tracking the given charging order.
    void startFor(const charging::client::ChargingStatus& initial);
    // 整合壳层（HomeShell）内使用：隐藏页内返回按钮（全局顶部导航负责返回）。
    void setEmbedded(bool embedded);

signals:
    void backRequested();
    void settlementRequested(const charging::client::ChargingStatus& stopped);

private slots:
    void onStatusLoaded(const charging::client::ChargingStatus& status);
    void onStopCompleted(const charging::client::ChargingStatus& status);
    void onOperationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void buildUi();
    void render(const charging::client::ChargingStatus& status);
    void setStopBusy(bool busy);
    void requestStop();

    ChargingService* service_ = nullptr;
    ChargingStatus latest_;
    bool hasData_ = false;

    QLabel* stationLabel_ = nullptr;
    QLabel* metaLabel_ = nullptr;
    StatusTag* statusTag_ = nullptr;
    QLabel* powerValueLabel_ = nullptr;
    QLabel* powerCaptionLabel_ = nullptr;
    QLabel* energyValueLabel_ = nullptr;
    QLabel* durationValueLabel_ = nullptr;
    QLabel* estimateValueLabel_ = nullptr;
    QLabel* updatedLabel_ = nullptr;
    ChargingPulse* pulse_ = nullptr;
    Card* heroCard_ = nullptr;
    NoticePanel* statusNotice_ = nullptr;
    ActionButton* backButton_ = nullptr; // 页内返回（嵌入壳层时隐藏）
    ActionBar* stopBar_ = nullptr;       // 底部操作条（停止充电）
    ActionButton* stopButton_ = nullptr;
    LoadingOverlay* overlay_ = nullptr;
};

} // namespace charging::client
