#pragma once

#include "charging/common/protocol/frame_codec.h"
#include "charging/common/protocol/protocol.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QtGlobal>

class QTcpSocket;
class QTimer;

namespace charging::client::network {

class ClientConnection final : public QObject
{
    Q_OBJECT

public:
    explicit ClientConnection(QObject* parent = nullptr);
    ClientConnection(const QString& hostName, quint16 port, QObject* parent = nullptr);

    QString sendRequest(const QString& type, const QJsonObject& data = {});
    bool isConnected() const;
    QString hostName() const;
    quint16 port() const;

signals:
    void connectionStateChanged(bool connected);
    void responseReceived(const charging::protocol::ResponseEnvelope& response);
    void requestFailed(const QString& requestId, const QString& errorCode, const QString& message);

private slots:
    void handleConnected();
    void handleDisconnected();
    void handleReadyRead();
    void handleSocketError();

private:
    struct PendingRequest
    {
        QString type;
        QByteArray frame;
        QTimer* timeoutTimer = nullptr;
        bool sent = false;
    };

    void ensureConnected();
    void sendPendingRequests();
    void failRequest(const QString& requestId, const QString& errorCode, const QString& message);
    void failAllRequests(const QString& errorCode, const QString& message);
    void closeForProtocolError(const charging::protocol::ProtocolError& error);

    QString hostName_;
    quint16 port_ = 0;
    QTcpSocket* socket_ = nullptr;
    charging::protocol::FrameDecoder frameDecoder_;
    QHash<QString, PendingRequest> pendingRequests_;
    QQueue<QString> unsentRequestIds_;
};

} // namespace charging::client::network
