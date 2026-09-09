#pragma once
#include <QObject>
#include <QVariantMap>

namespace charging::client { class IRequestTransport; namespace network { class ClientConnection; } }
namespace charging::qml {
// Session-scoped facade; UI request IDs are separate from wire correlation.
class WorkflowBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap queue READ queue NOTIFY queueChanged)
public:
    WorkflowBridge(charging::client::IRequestTransport* transport,
                   charging::client::network::ClientConnection* connection, bool active,
                   QObject* parent = nullptr);
    Q_INVOKABLE QString request(const QString& type, const QVariantMap& data = {});
    QVariantMap queue() const { return queue_; }
signals:
    void finished(const QString& requestId, const QString& type, bool success,
                  const QVariantMap& data, const QVariantMap& error);
    void changed();
    void queueChanged();
    void callAvailable(const QVariantMap& item);
private:
    void refreshQueue();
    void subscribe();
    charging::client::IRequestTransport* transport_;
    QVariantMap queue_;
    bool active_ = false;
    bool queuePending_ = false;
    bool subscribed_ = false;
    bool subscriptionPending_ = false;
    QString announcedCall_;
};
}
