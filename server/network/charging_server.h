#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>

class QHostAddress;
class QTcpSocket;

namespace charging::server {

class ChargingServer final : public QObject
{
    Q_OBJECT

public:
    explicit ChargingServer(QObject* parent = nullptr);

    bool listen(const QHostAddress& address, quint16 port);
    bool isListening() const;
    quint16 serverPort() const;
    QString errorString() const;
    int clientCount() const;

signals:
    void listening(quint16 port);
    void clientConnected(QTcpSocket* socket);
    void clientCountChanged(int count);

private slots:
    void handleNewConnections();
    void handleClientDisconnected();

private:
    QTcpServer tcpServer_;
    QSet<QTcpSocket*> clients_;
};

} // namespace charging::server
