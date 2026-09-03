#include "network/client_connection.h"

#include <QAbstractSocket>
#include <QList>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

namespace charging::client::network {

namespace {

constexpr quint16 kDefaultServerPort = 9527;
constexpr int kRequestTimeoutMilliseconds = 10000;

QString connectionErrorCode()
{
    return QString::fromLatin1(charging::protocol::error_code::kConnectionError);
}

} // namespace

ClientConnection::ClientConnection(QObject* parent)
    : ClientConnection(QStringLiteral("127.0.0.1"), kDefaultServerPort, parent)
{
}

ClientConnection::ClientConnection(const QString& hostName, quint16 port, QObject* parent)
    : QObject(parent), hostName_(hostName), port_(port), socket_(new QTcpSocket(this))
{
    connect(socket_, &QTcpSocket::connected, this, &ClientConnection::handleConnected);
    connect(socket_, &QTcpSocket::disconnected, this, &ClientConnection::handleDisconnected);
    connect(socket_, &QTcpSocket::readyRead, this, &ClientConnection::handleReadyRead);
    connect(socket_, &QTcpSocket::errorOccurred, this, &ClientConnection::handleSocketError);
}

QString ClientConnection::sendRequest(const QString& type, const QJsonObject& data)
{
    charging::protocol::RequestEnvelope request;
    request.type = type;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.data = data;

    QByteArray frame;
    charging::protocol::ProtocolError error;
    if (!charging::protocol::encodeFrame(charging::protocol::serializePayload(request), &frame,
                                         &error)) {
        QTimer::singleShot(0, this, [this, request, error]() {
            emit requestFailed(request.requestId, error.code, error.message);
        });
        return request.requestId;
    }

    auto* timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    const QString requestId = request.requestId;
    connect(timeoutTimer, &QTimer::timeout, this, [this, requestId]() {
        failRequest(requestId,
                    QString::fromLatin1(charging::protocol::error_code::kRequestTimeout),
                    tr("请求超时，请检查服务端后重试"));
    });

    PendingRequest pending;
    pending.type = type;
    pending.frame = frame;
    pending.timeoutTimer = timeoutTimer;
    pendingRequests_.insert(requestId, pending);
    timeoutTimer->start(kRequestTimeoutMilliseconds);

    if (isConnected()) {
        sendPendingRequests();
    } else {
        ensureConnected();
    }
    return requestId;
}

bool ClientConnection::isConnected() const
{
    return socket_->state() == QAbstractSocket::ConnectedState;
}

QString ClientConnection::hostName() const
{
    return hostName_;
}

quint16 ClientConnection::port() const
{
    return port_;
}

void ClientConnection::handleConnected()
{
    frameDecoder_.reset();
    emit connectionStateChanged(true);
    sendPendingRequests();
}

void ClientConnection::handleDisconnected()
{
    frameDecoder_.reset();
    emit connectionStateChanged(false);
    if (!pendingRequests_.isEmpty()) {
        failAllRequests(connectionErrorCode(), tr("与服务端的连接已断开"));
    }
}

void ClientConnection::handleReadyRead()
{
    QList<QByteArray> payloads;
    charging::protocol::ProtocolError frameError;
    if (!frameDecoder_.append(socket_->readAll(), &payloads, &frameError)) {
        closeForProtocolError(frameError);
        return;
    }

    for (const QByteArray& payload : payloads) {
        charging::protocol::ResponseEnvelope response;
        charging::protocol::ProtocolError parseError;
        if (!charging::protocol::parseResponsePayload(payload, &response, &parseError)) {
            closeForProtocolError(parseError);
            return;
        }

        const auto iterator = pendingRequests_.find(response.requestId);
        if (iterator == pendingRequests_.end()) {
            continue;
        }
        if (iterator->type != response.type) {
            charging::protocol::ProtocolError mismatchError;
            mismatchError.code =
                QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope);
            mismatchError.message = tr("响应类型与原请求不匹配");
            closeForProtocolError(mismatchError);
            return;
        }

        if (iterator->timeoutTimer != nullptr) {
            iterator->timeoutTimer->stop();
            iterator->timeoutTimer->deleteLater();
        }
        pendingRequests_.erase(iterator);
        emit responseReceived(response);
    }
}

void ClientConnection::handleSocketError()
{
    if (!pendingRequests_.isEmpty()) {
        failAllRequests(connectionErrorCode(), tr("无法连接服务端：%1").arg(socket_->errorString()));
    }
}

void ClientConnection::ensureConnected()
{
    if (socket_->state() == QAbstractSocket::UnconnectedState) {
        socket_->connectToHost(hostName_, port_);
    }
}

void ClientConnection::sendPendingRequests()
{
    if (!isConnected()) {
        return;
    }

    for (auto iterator = pendingRequests_.begin(); iterator != pendingRequests_.end(); ++iterator) {
        if (iterator->sent) {
            continue;
        }
        const qint64 acceptedBytes = socket_->write(iterator->frame);
        if (acceptedBytes != iterator->frame.size()) {
            failAllRequests(connectionErrorCode(), tr("发送请求失败：%1").arg(socket_->errorString()));
            socket_->abort();
            return;
        }
        iterator->sent = true;
    }
}

void ClientConnection::failRequest(const QString& requestId, const QString& errorCode,
                                   const QString& message)
{
    const auto iterator = pendingRequests_.find(requestId);
    if (iterator == pendingRequests_.end()) {
        return;
    }
    if (iterator->timeoutTimer != nullptr) {
        iterator->timeoutTimer->stop();
        iterator->timeoutTimer->deleteLater();
    }
    pendingRequests_.erase(iterator);

    // A socket write can fail inside sendRequest(). Delivering the terminal
    // signal on the next event-loop turn lets callers store the returned
    // requestId before they receive the failure.
    QTimer::singleShot(0, this, [this, requestId, errorCode, message]() {
        emit requestFailed(requestId, errorCode, message);
    });
}

void ClientConnection::failAllRequests(const QString& errorCode, const QString& message)
{
    const QList<QString> requestIds = pendingRequests_.keys();
    for (const QString& requestId : requestIds) {
        failRequest(requestId, errorCode, message);
    }
}

void ClientConnection::closeForProtocolError(const charging::protocol::ProtocolError& error)
{
    const QString code = error.code.isEmpty()
                             ? QString::fromLatin1(charging::protocol::error_code::kInvalidEnvelope)
                             : error.code;
    const QString message = error.message.isEmpty() ? tr("服务端响应格式错误") : error.message;
    failAllRequests(code, message);
    socket_->abort();
}

} // namespace charging::client::network
