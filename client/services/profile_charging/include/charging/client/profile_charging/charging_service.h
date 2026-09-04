#pragma once

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/model/models.h"

#include <QObject>
#include <QString>

class QTimer;

namespace charging::client {

// One live snapshot of a charging session plus the joined display fields.
// All values are server-provided; the UI never computes billing locally.
// Mirrors the frozen §8.4 shape: the order (live durationSeconds/energyWh/
// amountCents while CHARGING) plus the sibling currentPowerWatts.
struct ChargingStatus
{
    charging::model::Order order;
    QString stationName; // TODO(contract): join fields still pending the leader
    QString chargerCode;
    qint64 powerWatts = 0;
    bool powerKnown = false; // false only if the envelope lacks currentPowerWatts
};

// Use cases around an active charging session: live status polling, stop,
// and payment of the resulting order. Polling lives here (not in the page)
// so pages stay pure presentation.
class ChargingService final : public QObject
{
    Q_OBJECT

public:
    // Poll cadence is a client UX choice: 1s keeps the duration counter
    // ticking like a stopwatch (server-side load preference still
    // TODO(contract) once the leader's real endpoint is live).
    static constexpr int kStatusPollIntervalMs = 1000;

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
    // Parses a GET_CHARGING_STATUS / STOP_CHARGING success envelope
    // (§8.4/8.5): { order: {...}, currentPowerWatts: N, stationName?,
    // chargerCode? }. Returns false when the order sub-object is missing or
    // fails model validation.
    static bool parseStatus(const QJsonObject& data, ChargingStatus* outStatus,
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
