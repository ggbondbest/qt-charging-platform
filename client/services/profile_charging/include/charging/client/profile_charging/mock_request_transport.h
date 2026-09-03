#pragma once

#include "charging/client/profile_charging/i_request_transport.h"
#include "charging/common/model/models.h"

#include <QHash>
#include <QObject>
#include <QVector>

namespace charging::client {

// TEMPORARY MOCK — visual development only.
//
// This class replaces the real ClientConnection until the leader releases the
// production network layer. It keeps an in-memory user, responds to the
// candidate-v1 wallet actions with contract-shaped JSON payloads (parsed back
// through common model_json so field names stay honest), and simulates
// latency asynchronously with QTimer. It must not be linked into the shipped
// client app; delete this file when the real transport adapter lands.
class MockRequestTransport final : public QObject, public IRequestTransport
{
    Q_OBJECT

public:
    MockRequestTransport();

    void send(const QString& type, const QJsonObject& data,
              const ResponseCallback& callback) override;

    // Debug hooks for manual demo runs.
    void setNextFailure(const QString& code, int times = 1);
    // Test/demo helper: sets the in-memory balance so the insufficient-balance
    // path of PAY_ORDER can be exercised without a real wallet drain.
    void drainBalanceTo(qint64 cents);

private:
    void handleRequest(const QString& type, const QJsonObject& data,
                       const ResponseCallback& callback);
    void seedDemoData();
    void seedDemoOrders();
    charging::model::Order* findOrder(qint64 orderId);
    QJsonObject buildStatusPayload(const charging::model::Order& order, qint64 powerWatts,
                                   qint64 energyWh, qint64 durationSeconds) const;
    QJsonObject payResultPayload(const charging::model::Order& order) const;

    charging::model::User user_;
    QVector<charging::model::RechargeRecord> records_; // newest first
    QVector<charging::model::Order> orders_; // newest first
    QHash<qint64, QPair<QString, QString>> chargerDisplays_; // chargerId -> (station, code)
    qint64 nextRecordId_ = 1;
    qint64 nextTransactionSeq_ = 1;
    QString nextFailureCode_;
    int nextFailureRemaining_ = 0;
};

} // namespace charging::client
