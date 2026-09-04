#include "charging_server.h"

#include <QHostAddress>
#include <QTcpSocket>

namespace charging::server {

ChargingServer::ChargingServer(QObject* parent) : QObject(parent)
{
    connect(&tcpServer_, &QTcpServer::newConnection, this, &ChargingServer::handleNewConnections);
}

bool ChargingServer::listen(const QHostAddress& address, quint16 port)
{
    if (!tcpServer_.listen(address, port)) {
        return false;
    }

    emit listening(tcpServer_.serverPort());
    return true;
}

bool ChargingServer::isListening() const
{
    return tcpServer_.isListening();
}

quint16 ChargingServer::serverPort() const
{
    return tcpServer_.serverPort();
}

QString ChargingServer::errorString() const
{
    return tcpServer_.errorString();
}

int ChargingServer::clientCount() const
{
    return clients_.size();
}

void ChargingServer::handleNewConnections()
{
    while (tcpServer_.hasPendingConnections()) {
        QTcpSocket* connection = tcpServer_.nextPendingConnection();
        if (connection == nullptr) {
            continue;
        }

        connection->setParent(this);
        clients_.insert(connection);
        connect(connection, &QTcpSocket::disconnected, this,
                &ChargingServer::handleClientDisconnected);
        connect(connection, &QTcpSocket::disconnected, connection, &QObject::deleteLater);

        emit clientConnected(connection);
        emit clientCountChanged(clients_.size());
    }
}

void ChargingServer::handleClientDisconnected()
{
    auto* connection = qobject_cast<QTcpSocket*>(sender());
    if (connection != nullptr && clients_.remove(connection) > 0) {
        emit clientCountChanged(clients_.size());
    }
}

} // namespace charging::server
