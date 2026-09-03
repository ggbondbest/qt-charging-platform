#include "charging/common/protocol/frame_codec.h"
#include "charging/common/protocol/protocol.h"
#include "charging_server.h"
#include "network/client_connection.h"
#include "request_dispatcher.h"
#include "user_repository.h"
#include "user_service.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {

using charging::client::network::ClientConnection;

void verifyEachRequestFailedOnce(const QSignalSpy& failures, const QStringList& requestIds)
{
    QCOMPARE(failures.size(), requestIds.size());

    const QString expectedCode =
        QString::fromLatin1(charging::protocol::error_code::kConnectionError);
    QHash<QString, int> counts;
    for (const QList<QVariant>& arguments : failures) {
        QCOMPARE(arguments.size(), 3);
        const QString failedRequestId = arguments.at(0).toString();
        QCOMPARE(arguments.at(1).toString(), expectedCode);
        QVERIFY2(requestIds.contains(failedRequestId), qPrintable(failedRequestId));
        ++counts[failedRequestId];
    }

    for (const QString& requestId : requestIds) {
        QCOMPARE(counts.value(requestId), 1);
    }
}

class MalformedFrameServerFixture final
{
public:
    MalformedFrameServerFixture()
        : repository_(QSqlDatabase()), service_(&repository_), dispatcher_(&service_)
    {
        server_.setRequestDispatcher(&dispatcher_);
    }

    bool listen()
    {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    charging::server::ChargingServer& server()
    {
        return server_;
    }

private:
    charging::server::UserRepository repository_;
    charging::server::UserService service_;
    charging::server::RequestDispatcher dispatcher_;
    charging::server::ChargingServer server_;
};

} // namespace

class NetworkResilienceTest final : public QObject
{
    Q_OBJECT

private slots:
    void queuedRequestsAreWrittenInFifoOrderAfterConnecting();
    void refusedConnectionFailsEveryPendingRequestExactlyOnce();
    void disconnectFailsEveryPendingRequestExactlyOnce();
    void malformedClientFrameClosesOnlyThatSession_data();
    void malformedClientFrameClosesOnlyThatSession();
};

void NetworkResilienceTest::queuedRequestsAreWrittenInFifoOrderAfterConnecting()
{
    QTcpServer rawServer;
    QVERIFY2(rawServer.listen(QHostAddress::LocalHost, 0), qPrintable(rawServer.errorString()));

    charging::protocol::FrameDecoder decoder;
    QList<QByteArray> receivedPayloads;
    charging::protocol::ProtocolError decoderError;
    bool decodingSucceeded = true;

    connect(&rawServer, &QTcpServer::newConnection, this, [&]() {
        while (rawServer.hasPendingConnections()) {
            QTcpSocket* const acceptedSocket = rawServer.nextPendingConnection();
            QVERIFY(acceptedSocket != nullptr);
            const auto drainSocket = [&, acceptedSocket]() {
                QList<QByteArray> completedPayloads;
                charging::protocol::ProtocolError error;
                if (!decoder.append(acceptedSocket->readAll(), &completedPayloads, &error)) {
                    decodingSucceeded = false;
                    decoderError = error;
                    return;
                }
                receivedPayloads.append(completedPayloads);
            };
            connect(acceptedSocket, &QTcpSocket::readyRead, this, drainSocket);
            drainSocket();
        }
    });

    ClientConnection connection(QStringLiteral("127.0.0.1"), rawServer.serverPort());
    QVERIFY(!connection.isConnected());

    const QStringList expectedTypes = {
        QStringLiteral("FIFO_FIRST"),
        QStringLiteral("FIFO_SECOND"),
        QStringLiteral("FIFO_THIRD"),
    };
    QStringList expectedRequestIds;
    for (const QString& type : expectedTypes) {
        expectedRequestIds.append(connection.sendRequest(type));
    }

    QTRY_COMPARE(receivedPayloads.size(), expectedTypes.size());
    QVERIFY2(decodingSucceeded, qPrintable(decoderError.message));

    QStringList receivedTypes;
    QStringList receivedRequestIds;
    for (const QByteArray& payload : receivedPayloads) {
        charging::protocol::RequestEnvelope request;
        charging::protocol::ProtocolError parseError;
        QVERIFY2(charging::protocol::parseRequestPayload(payload, &request, &parseError),
                 qPrintable(parseError.message));
        receivedTypes.append(request.type);
        receivedRequestIds.append(request.requestId);
    }

    QCOMPARE(receivedTypes, expectedTypes);
    QCOMPARE(receivedRequestIds, expectedRequestIds);
}

void NetworkResilienceTest::refusedConnectionFailsEveryPendingRequestExactlyOnce()
{
    QTcpServer portReservation;
    QVERIFY2(portReservation.listen(QHostAddress::LocalHost, 0),
             qPrintable(portReservation.errorString()));
    const quint16 unusedPort = portReservation.serverPort();
    portReservation.close();

    ClientConnection connection(QStringLiteral("127.0.0.1"), unusedPort);
    QSignalSpy failures(&connection, &ClientConnection::requestFailed);
    QVERIFY(failures.isValid());

    const QStringList requestIds = {
        connection.sendRequest(QStringLiteral("REFUSED_FIRST")),
        connection.sendRequest(QStringLiteral("REFUSED_SECOND")),
        connection.sendRequest(QStringLiteral("REFUSED_THIRD")),
    };

    QTRY_COMPARE(failures.size(), requestIds.size());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    verifyEachRequestFailedOnce(failures, requestIds);
}

void NetworkResilienceTest::disconnectFailsEveryPendingRequestExactlyOnce()
{
    QTcpServer rawServer;
    QVERIFY2(rawServer.listen(QHostAddress::LocalHost, 0), qPrintable(rawServer.errorString()));

    charging::protocol::FrameDecoder decoder;
    QList<QByteArray> receivedPayloads;
    bool decodingSucceeded = true;
    QTcpSocket* acceptedSocket = nullptr;
    connect(&rawServer, &QTcpServer::newConnection, this, [&]() {
        acceptedSocket = rawServer.nextPendingConnection();
        QVERIFY(acceptedSocket != nullptr);
        const auto drainSocket = [&]() {
            QList<QByteArray> completedPayloads;
            charging::protocol::ProtocolError error;
            if (!decoder.append(acceptedSocket->readAll(), &completedPayloads, &error)) {
                decodingSucceeded = false;
                return;
            }
            receivedPayloads.append(completedPayloads);
        };
        connect(acceptedSocket, &QTcpSocket::readyRead, this, drainSocket);
        drainSocket();
    });

    ClientConnection connection(QStringLiteral("127.0.0.1"), rawServer.serverPort());
    QSignalSpy failures(&connection, &ClientConnection::requestFailed);
    QVERIFY(failures.isValid());

    const QStringList requestIds = {
        connection.sendRequest(QStringLiteral("DISCONNECT_FIRST")),
        connection.sendRequest(QStringLiteral("DISCONNECT_SECOND")),
        connection.sendRequest(QStringLiteral("DISCONNECT_THIRD")),
    };

    QTRY_COMPARE(receivedPayloads.size(), requestIds.size());
    QVERIFY(decodingSucceeded);
    QVERIFY(acceptedSocket != nullptr);
    acceptedSocket->abort();

    QTRY_COMPARE(failures.size(), requestIds.size());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    verifyEachRequestFailedOnce(failures, requestIds);
}

void NetworkResilienceTest::malformedClientFrameClosesOnlyThatSession_data()
{
    QTest::addColumn<QByteArray>("badFrame");

    QTest::newRow("zero-length-frame") << QByteArray::fromHex("00000000");
    QTest::newRow("oversized-frame") << QByteArray::fromHex("00100001");

    QByteArray invalidJsonFrame;
    charging::protocol::ProtocolError encodeError;
    QVERIFY(charging::protocol::encodeFrame(QByteArrayLiteral("{not-json"), &invalidJsonFrame,
                                            &encodeError));
    QTest::newRow("invalid-json-without-identity") << invalidJsonFrame;
}

void NetworkResilienceTest::malformedClientFrameClosesOnlyThatSession()
{
    QFETCH(QByteArray, badFrame);

    MalformedFrameServerFixture fixture;
    QVERIFY2(fixture.listen(), qPrintable(fixture.server().errorString()));

    QTcpSocket maliciousClient;
    maliciousClient.connectToHost(QHostAddress::LocalHost, fixture.server().serverPort());
    QVERIFY2(maliciousClient.waitForConnected(1000), qPrintable(maliciousClient.errorString()));
    QTRY_COMPARE(fixture.server().clientCount(), 1);

    QCOMPARE(maliciousClient.write(badFrame), static_cast<qint64>(badFrame.size()));
    QVERIFY(maliciousClient.flush());
    QTRY_COMPARE(maliciousClient.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(fixture.server().clientCount(), 0);
    QVERIFY(fixture.server().isListening());

    QTcpSocket healthyClient;
    healthyClient.connectToHost(QHostAddress::LocalHost, fixture.server().serverPort());
    QVERIFY2(healthyClient.waitForConnected(1000), qPrintable(healthyClient.errorString()));
    QTRY_COMPARE(fixture.server().clientCount(), 1);
    QVERIFY(fixture.server().isListening());

    healthyClient.disconnectFromHost();
    if (healthyClient.state() != QAbstractSocket::UnconnectedState) {
        QVERIFY(healthyClient.waitForDisconnected(1000));
    }
    QTRY_COMPARE(fixture.server().clientCount(), 0);
}

QTEST_GUILESS_MAIN(NetworkResilienceTest)

#include "tst_network_resilience.moc"
