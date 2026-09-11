#include "workflow_bridge.h"
#include "charging/client/profile_charging/i_request_transport.h"
#include "network/client_connection.h"
#include <QSet>
#include <QTimer>
#include <QUuid>

namespace charging::qml {
WorkflowBridge::WorkflowBridge(charging::client::IRequestTransport* transport,
    charging::client::network::ClientConnection* connection, bool active, QObject* parent)
    : QObject(parent), transport_(transport), active_(active)
{
    if (!active_) return;
    connect(connection, &charging::client::network::ClientConnection::eventReceived, this,
            [this](const QString& type, const QJsonObject&) {
        if (type != QStringLiteral("WORKFLOW_CHANGED")) return;
        refreshQueue();
        emit changed();
    });
    QTimer::singleShot(0, this, [this] {
        subscribe();
        refreshQueue();
    });
    // Also recovers missed invalidations after a slow UI / dropped output.
    auto* fallback = new QTimer(this);
    fallback->setObjectName(QStringLiteral("workflowRefreshTimer"));
    fallback->setInterval(10000);
    connect(fallback, &QTimer::timeout, this, [this] {
        subscribe();
        refreshQueue();
        emit changed();
    });
    fallback->start();
}

void WorkflowBridge::subscribe()
{
    if (!active_ || subscribed_ || subscriptionPending_) return;
    subscriptionPending_ = true;
    transport_->sendFor(this, QStringLiteral("WORKFLOW_SUBSCRIBE"), {},
        [this](bool ok, const QJsonObject&, const charging::protocol::ProtocolError&) {
        subscriptionPending_ = false;
        subscribed_ = ok;
    });
}

QString WorkflowBridge::request(const QString& type, const QVariantMap& data)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    static const QSet<QString> allowed = {QStringLiteral("QUEUE_JOIN"), QStringLiteral("QUEUE_GET_MINE"),
        QStringLiteral("QUEUE_LEAVE"), QStringLiteral("QUEUE_CONFIRM"), QStringLiteral("REPAIR_SUBMIT"),
        QStringLiteral("REPAIR_GET_MINE"), QStringLiteral("REPAIR_GET")};
    if (!active_ || !allowed.contains(type)) {
        QTimer::singleShot(0, this, [this, id, type] {
            emit finished(id, type, false, {}, {{QStringLiteral("code"), QStringLiteral("UNAVAILABLE")},
                {QStringLiteral("message"), tr("请连接服务器并登录后使用此功能")}});
        });
        return id;
    }
    transport_->sendFor(this, type, QJsonObject::fromVariantMap(data),
        [this, id, type](bool ok, const QJsonObject& result, const charging::protocol::ProtocolError& error) {
        emit finished(id, type, ok, result.toVariantMap(),
            {{QStringLiteral("code"), error.code}, {QStringLiteral("message"), error.message}});
        if (ok && type != QStringLiteral("QUEUE_GET_MINE") && type != QStringLiteral("REPAIR_GET_MINE")
            && type != QStringLiteral("REPAIR_GET")) { refreshQueue(); emit changed(); }
    });
    return id;
}

void WorkflowBridge::refreshQueue()
{
    if (!active_ || queuePending_) return;
    queuePending_ = true;
    transport_->sendFor(this, QStringLiteral("QUEUE_GET_MINE"), {},
        [this](bool ok, const QJsonObject& data, const charging::protocol::ProtocolError&) {
        queuePending_ = false;
        if (!ok) return;
        const QVariantMap next = data.value(QStringLiteral("item")).toObject().toVariantMap();
        if (next != queue_) { queue_ = next; emit queueChanged(); }
        if (queue_.value(QStringLiteral("status")).toString() == QStringLiteral("CALLED")) {
            const QString identity = queue_.value(QStringLiteral("id")).toString()
                + queue_.value(QStringLiteral("calledAt")).toString();
            if (announcedCall_ != identity) { announcedCall_ = identity; emit callAvailable(queue_); }
        }
    });
}
}
