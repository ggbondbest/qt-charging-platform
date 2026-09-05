#include "charging/client/profile_charging/network_request_transport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTimer>

namespace charging::client {
namespace {
void deliver(const IRequestTransport::ResponseCallback& callback, bool success,
             const QJsonObject& data, const charging::protocol::ProtocolError& error)
{
    QTimer::singleShot(0, QCoreApplication::instance(), [callback, success, data, error]() {
        if (callback) callback(success, data, error);
    });
}
charging::protocol::ProtocolError disconnectedError()
{
    charging::protocol::ProtocolError error;
    error.code = charging::protocol::error_code::kConnectionError;
    error.message = QStringLiteral("连接已失效，请重新登录；充值结果不明时请重试原交易");
    return error;
}
} // namespace

NetworkRequestTransport::NetworkRequestTransport(network::ClientConnection* connection,
                                                 qint64 authenticatedUserId, QObject* parent)
    : QObject(parent), connection_(connection), userId_(authenticatedUserId),
      authenticated_(connection && connection->isConnected() && authenticatedUserId > 0)
{
    if (!connection) return;
    connect(connection, &network::ClientConnection::responseReceived, this,
            [this](const charging::protocol::ResponseEnvelope& response) {
        if (response.type == QLatin1String(charging::protocol::request_type::kUserLogin)) {
            authenticated_ = response.success
                && response.data.value("user").toObject().value("id").toString() == QString::number(userId_);
        }
        finish(response.requestId, response.success, response.data, response.error);
    });
    connect(connection, &network::ClientConnection::requestFailed, this,
            [this](const QString& id, const QString& code, const QString& message) {
        charging::protocol::ProtocolError error; error.code = code; error.message = message;
        finish(id, false, {}, error);
    });
    connect(connection, &network::ClientConnection::connectionStateChanged, this, [this](bool connected) {
        if (!connected) { authenticated_ = false; failAll(); }
    });
    connect(connection, &QObject::destroyed, this, [this]() {
        authenticated_ = false; failAll();
    });
}
NetworkRequestTransport::~NetworkRequestTransport() { failAll(); }

QString NetworkRequestTransport::persistenceScope() const
{
    if (!connection_ || userId_ <= 0) return {};
    const QByteArray identity = (connection_->hostName() + ":" + QString::number(connection_->port())
                                  + "/" + QString::number(userId_)).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

void NetworkRequestTransport::send(const QString& type, const QJsonObject& data,
                                   const ResponseCallback& callback)
{
    if (!connection_ || !connection_->isConnected()) {
        deliver(callback, false, {}, disconnectedError());
        return;
    }
    if (!authenticated_) {
        charging::protocol::ProtocolError error;
        error.code = charging::protocol::error_code::kUnauthorized;
        error.message = QStringLiteral("请重新登录");
        deliver(callback, false, {}, error);
        return;
    }
    pending_.insert(connection_->sendRequest(type, data), callback);
}

void NetworkRequestTransport::finish(const QString& id, bool success, const QJsonObject& data,
                                     const charging::protocol::ProtocolError& error)
{
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    const auto callback = it.value();
    pending_.erase(it);
    deliver(callback, success, data, error);
}
void NetworkRequestTransport::failAll()
{
    const auto callbacks = pending_;
    pending_.clear();
    for (const auto& callback : callbacks) deliver(callback, false, {}, disconnectedError());
}
} // namespace charging::client
