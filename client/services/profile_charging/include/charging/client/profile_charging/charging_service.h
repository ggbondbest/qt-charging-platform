#pragma once

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/model/models.h"

#include <QObject>
#include <QString>

class QTimer;

namespace charging::client {

// One live snapshot of a charging session plus the joined display fields.
// All values are server-provided; the UI never computes billing locally.
struct ChargingStatus
{
    charging::model::Order order;
    QString stationName;
    QString chargerCode;
    qint64 powerWatts = 0;
    bool powerKnown = false; // TODO(contract): currentPowerWatts not frozen yet
    qint64 estimatedAmountCents = 0;
};

// Use cases around an active charging session: live status polling, stop,
// and payment of the resulting order. Polling lives here (not in the page)
// so pages stay pure presentation.
class ChargingService final : public QObject
{
    Q_OBJECT

public:
    static constexpr int kStatusPollIntervalMs = 2000;

    explicit ChargingService(IRequestTransport* transport, QObject* parent = nullptr);
    ~ChargingService() override;

    // Begins periodic GET_CHARGING_STATUS for the order (one immediate
    // fetch). Safe to call again with the same/different order.
    void startTracking(qint64 orderId);
    void stopTracking();
    bool isTracking() const;

    void fetchStatusNow(); // manual retry; ignored while a poll is in flight

    void stopCharging();        // STOP_CHARGING for the tracked order
    bool isStoppingCharging() const;
    void payOrder(qint64 orderId); // PAY_ORDER (server re-validates balance)
    bool isPaying() const;

signals:
    void statusLoaded(const charging::client::ChargingStatus& status);
    void stopCompleted(const charging::client::ChargingStatus& status);
    void paymentCompleted(qint64 amountCents, qint64 balanceAfterCents);
    void operationFailed(const QString& type, const charging::protocol::ProtocolError& error);

private:
    void handlePollTick();
    static bool parseStatus(const QJsonObject& object, ChargingStatus* outStatus,
                            QString* errorMessage);

    IRequestTransport* transport_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    qint64 trackedOrderId_ = 0;
    bool fetchingStatus_ = false;
    bool stopping_ = false;
    bool paying_ = false;
};

} // namespace charging::client

// Enables ChargingStatus in queued signal/slot connections and QSignalSpy.
Q_DECLARE_METATYPE(charging::client::ChargingStatus)
