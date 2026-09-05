#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>

class QHostAddress;
class QTcpSocket;

namespace charging::server {

class RequestDispatcher;

class ChargingServer final : public QObject
{
    Q_OBJECT

public:
    explicit ChargingServer(QObject* parent = nullptr);
    ~ChargingServer() override;

    // The dispatcher is application-owned and must outlive this server. It can
    // only be set before listen() starts accepting connections.
    void setRequestDispatcher(RequestDispatcher* dispatcher);

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
    RequestDispatcher* dispatcher_ = nullptr;
};

} // namespace charging::server
