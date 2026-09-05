#include "client_session.h"

#include "charging/common/protocol/frame_codec.h"
#include "charging/common/protocol/protocol.h"
#include "request_dispatcher.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QTcpSocket>
#include <QThread>

#include <exception>

namespace charging::server {

namespace {

charging::protocol::RequestEnvelope bestEffortRequestIdentity(const QByteArray& payload)
{
    charging::protocol::RequestEnvelope request;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return request;
    }

    const QJsonObject object = document.object();
    const QJsonValue type = object.value(QStringLiteral("type"));
    const QJsonValue requestId = object.value(QStringLiteral("requestId"));
    if (type.isString() && !type.toString().isEmpty() &&
        type.toString().size() <= charging::protocol::kMaxTypeLength) {
        request.type = type.toString();
    }
    if (requestId.isString() && !requestId.toString().isEmpty() &&
        requestId.toString().size() <= charging::protocol::kMaxRequestIdLength) {
        request.requestId = requestId.toString();
    }
    return request;
}

} // namespace

ClientSession::ClientSession(QTcpSocket* socket, RequestDispatcher* dispatcher, QObject* parent)
    : QObject(parent), socket_(socket), dispatcher_(dispatcher)
{
    Q_ASSERT(socket != nullptr);
    Q_ASSERT(dispatcher_ != nullptr);
    socket->setReadBufferSize(static_cast<qint64>(charging::protocol::kMaxPayloadBytes) + 4);
    connect(socket, &QTcpSocket::readyRead, this, &ClientSession::handleReadyRead);
}

SessionRole ClientSession::role() const
{
    return role_;
}

qint64 ClientSession::authenticatedUserId() const
{
    return authenticatedUserId_;
}

void ClientSession::handleReadyRead()
{
    if (socket_ == nullptr) {
        return;
    }

    QList<QByteArray> completedPayloads;
    charging::protocol::ProtocolError frameError;
    if (!decoder_.append(socket_->readAll(), &completedPayloads, &frameError)) {
        // A length error loses stream synchronisation, so this connection must
        // be closed instead of trying to continue at an unknown byte offset.
        qWarning().noquote() << "Closing client after framing error:" << frameError.code;
        socket_->abort();
        return;
    }

    for (const QByteArray& payload : completedPayloads) {
        // Shutdown finishes the current transaction, not the rest of a pipelined
        // batch. Unanswered writes are not automatically replayed by the client.
        if (QThread::currentThread()->isInterruptionRequested()) return;
        handlePayload(payload);
        if (socket_ == nullptr || socket_->state() == QAbstractSocket::UnconnectedState) {
            return;
        }
    }
}

void ClientSession::handlePayload(const QByteArray& payload)
{
    charging::protocol::RequestEnvelope request;
    charging::protocol::ProtocolError parseError;
    if (!charging::protocol::parseRequestPayload(payload, &request, &parseError)) {
        sendPayloadError(payload, parseError);
        return;
    }

    if (request.type == QString::fromLatin1(charging::protocol::request_type::kUserLogin)) {
        authenticatedUserId_ = 0;
        role_ = SessionRole::Anonymous;
    }
    qint64 userId = authenticatedUserId_;
    charging::protocol::ResponseEnvelope response;
    try {
        response = dispatcher_->dispatch(request, &userId);
    } catch (const std::exception&) {
        // Do not copy exception text into logs here: a future dependency may
        // include SQL, credentials, or a local path in what().
        qCritical().noquote() << "Unhandled standard request exception for" << request.requestId;
        charging::protocol::ProtocolError error;
        error.code = QString::fromLatin1(charging::protocol::error_code::kInternalError);
        error.message = QStringLiteral("服务端处理请求时发生内部错误");
        response = charging::protocol::makeErrorResponse(request, error);
    } catch (...) {
        qCritical().noquote() << "Unhandled unknown request exception for" << request.requestId;
        charging::protocol::ProtocolError error;
        error.code = QString::fromLatin1(charging::protocol::error_code::kInternalError);
        error.message = QStringLiteral("服务端处理请求时发生内部错误");
        response = charging::protocol::makeErrorResponse(request, error);
    }

    // A dispatcher implementation must not be able to break request/response
    // correlation for this session.
    response.type = request.type;
    response.requestId = request.requestId;
    if (response.success &&
        request.type == QString::fromLatin1(charging::protocol::request_type::kUserLogin) &&
        userId > 0) {
        authenticatedUserId_ = userId;
        role_ = SessionRole::User;
    }
    sendResponse(response);
}

void ClientSession::sendResponse(const charging::protocol::ResponseEnvelope& response)
{
    if (socket_ == nullptr) {
        return;
    }

    QByteArray frame;
    charging::protocol::ProtocolError frameError;
    const QByteArray payload = charging::protocol::serializePayload(response);
    if (!charging::protocol::encodeFrame(payload, &frame, &frameError)) {
        socket_->abort();
        return;
    }
    const qint64 acceptedBytes = socket_->write(frame);
    if (acceptedBytes != frame.size()) {
        socket_->abort();
    }
}

void ClientSession::sendPayloadError(const QByteArray& payload,
                                     const charging::protocol::ProtocolError& error)
{
    const charging::protocol::RequestEnvelope request = bestEffortRequestIdentity(payload);
    if (request.type.isEmpty() || request.requestId.isEmpty()) {
        // Without both correlation fields the peer cannot associate an error
        // response with a request. The stream itself is framed, but continuing
        // would only leave the real client request pending until timeout.
        qWarning().noquote() << "Closing client after request payload error without identity:"
                             << error.code;
        if (socket_ != nullptr) {
            socket_->abort();
        }
        return;
    }
    sendResponse(charging::protocol::makeErrorResponse(request, error));
}

} // namespace charging::server
