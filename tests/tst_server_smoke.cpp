#include "charging_server.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QtTest>

class ServerSmokeTest final : public QObject
{
    Q_OBJECT

private slots:
    void listensOnAvailableLocalPort();
    void tracksConnectedClients();
};

void ServerSmokeTest::listensOnAvailableLocalPort()
{
    charging::server::ChargingServer server;

    QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));
    QVERIFY(server.isListening());
    QVERIFY(server.serverPort() != 0);
}

void ServerSmokeTest::tracksConnectedClients()
{
    charging::server::ChargingServer server;
    QVERIFY2(server.listen(QHostAddress::LocalHost, 0), qPrintable(server.errorString()));

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QVERIFY2(socket.waitForConnected(1000), qPrintable(socket.errorString()));
    QTRY_COMPARE(server.clientCount(), 1);

    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        QVERIFY(socket.waitForDisconnected(1000));
    }
    QTRY_COMPARE(server.clientCount(), 0);
}

QTEST_GUILESS_MAIN(ServerSmokeTest)

#include "tst_server_smoke.moc"
