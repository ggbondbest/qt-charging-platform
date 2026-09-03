#pragma once

#include "charging/common/protocol/frame_codec.h"

#include <QObject>
#include <QPointer>
#include <QtGlobal>

class QTcpSocket;

namespace charging::protocol {
struct ProtocolError;
struct ResponseEnvelope;
} // namespace charging::protocol

namespace charging::server {

class RequestDispatcher;

enum class SessionRole
{
    Anonymous,
    User,
};

class ClientSession final : public QObject
{
    Q_OBJECT

public:
    ClientSession(QTcpSocket* socket, RequestDispatcher* dispatcher,
                  QObject* parent = nullptr);

    SessionRole role() const;
    qint64 authenticatedUserId() const;

private slots:
    void handleReadyRead();

private:
    void handlePayload(const QByteArray& payload);
    void sendResponse(const charging::protocol::ResponseEnvelope& response);
    void sendPayloadError(const QByteArray& payload,
                          const charging::protocol::ProtocolError& error);

    QPointer<QTcpSocket> socket_;
    RequestDispatcher* dispatcher_ = nullptr;
    charging::protocol::FrameDecoder decoder_;
    SessionRole role_ = SessionRole::Anonymous;
    qint64 authenticatedUserId_ = 0;
};

} // namespace charging::server
