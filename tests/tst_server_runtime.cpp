#include "server_runtime.h"
#include "database_connection.h"
#include "charging/common/protocol/frame_codec.h"
#include "charging/common/protocol/protocol.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using charging::server::ServerRuntime;
using namespace charging::protocol;

namespace {
QByteArray requestFrame(const QString& type, const QJsonObject& data, const QString& id)
{
    RequestEnvelope request;
    request.type = type;
    request.requestId = id;
    request.data = data;
    QByteArray frame;
    encodeFrame(serializePayload(request), &frame);
    return frame;
}

// Deliberately does NOT process the application's event loop. A server still
// using the main thread cannot accept/process these requests and will time out.
QList<ResponseEnvelope> receive(QTcpSocket& socket, int count = 1)
{
    QList<ResponseEnvelope> responses;
    FrameDecoder decoder;
    QElapsedTimer timer;
    timer.start();
    while (responses.size() < count && timer.elapsed() < 5000) {
        if (socket.bytesAvailable() == 0 &&
            !socket.waitForReadyRead(static_cast<int>(5000 - timer.elapsed()))) break;
        QList<QByteArray> payloads;
        if (!decoder.append(socket.readAll(), &payloads)) break;
        for (const auto& payload : payloads) {
            ResponseEnvelope response;
            if (!parseResponsePayload(payload, &response)) return {};
            responses.append(response);
        }
    }
    return responses;
}

ResponseEnvelope exchange(QTcpSocket& socket, const QString& type, const QJsonObject& data)
{
    socket.write(requestFrame(type, data, type));
    socket.waitForBytesWritten(1000);
    const auto responses = receive(socket);
    return responses.isEmpty() ? ResponseEnvelope{} : responses.first();
}
} // namespace

class ServerRuntimeTest final : public QObject
{
    Q_OBJECT
private slots:
    void workerProcessesOrderedRequestsAndWorkflow();
    void databaseWaitDoesNotBlockMainEventLoop();
    void startupFailuresReleaseConnections();
    void stopDuringStartupAndDestructorFallback();
};

void ServerRuntimeTest::workerProcessesOrderedRequestsAndWorkflow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto before = QSqlDatabase::connectionNames();
    const QString path = directory.filePath("worker.sqlite3");
    {
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        QSignalSpy stopped(&runtime, &ServerRuntime::stopped);
        QVERIFY(runtime.start(path, true, QHostAddress::LocalHost, 0));
        QVERIFY(!runtime.start(path, true, QHostAddress::LocalHost, 0));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(runtime.isListening());
        QTcpSocket first;
        first.connectToHost(QHostAddress::LocalHost, runtime.serverPort());
        QVERIFY(first.waitForConnected(1000));
        // Fragmented login followed by a pipelined authenticated query: session
        // identity must be established before the next request is dispatched.
        const auto login = requestFrame("USER_LOGIN", {{"phone", "13800138000"}}, "login");
        first.write(login.left(2));
        QVERIFY(first.waitForBytesWritten(1000));
        first.write(login.mid(2) + requestFrame("GET_USER_INFO", {}, "profile"));
        QVERIFY(first.waitForBytesWritten(1000));
        const auto replies = receive(first, 2);
        QCOMPARE(replies.size(), 2);
        QVERIFY(replies[0].success);
        QVERIFY(replies[1].success);
        QCOMPARE(replies[0].requestId, QString("login"));
        QCOMPARE(replies[1].requestId, QString("profile"));

        QTcpSocket second;
        second.connectToHost(QHostAddress::LocalHost, runtime.serverPort());
        QVERIFY(second.waitForConnected(1000));
        QCOMPARE(exchange(second, "GET_USER_INFO", {}).error.code, QString("UNAUTHORIZED"));
        QVERIFY(exchange(second, "USER_LOGIN", {{"phone", "13900139000"}}).success);
        QTRY_COMPARE(runtime.clientCount(), 2);

        const auto reserve = exchange(first, "RESERVE_CHARGER", {{"chargerId", "1"}});
        QVERIFY2(reserve.success, qPrintable(reserve.error.message));
        QVERIFY(!exchange(second, "RESERVE_CHARGER", {{"chargerId", "1"}}).success);
        const QString reservationId = reserve.data["reservation"].toObject()["id"].toString();
        const auto started = exchange(first, "START_CHARGING", {{"reservationId", reservationId}});
        QVERIFY(started.success);
        const QString orderId = started.data["order"].toObject()["id"].toString();
        const QJsonObject order{{"orderId", orderId}};
        QVERIFY(exchange(first, "GET_CHARGING_STATUS", order).success);
        QVERIFY(!exchange(second, "STOP_CHARGING", order).success);
        QVERIFY(exchange(first, "STOP_CHARGING", order).success);
        const auto paid = exchange(first, "PAY_ORDER", order);
        QVERIFY(paid.success);
        QCOMPARE(paid.data["order"].toObject()["status"].toString(), QString("COMPLETED"));
        QVERIFY(exchange(first, "PAY_ORDER", order).success);
        QVERIFY(exchange(first, "UPDATE_USER_INFO", {{"nickname", "线程测试"}}).success);

        runtime.stop();
        runtime.stop();
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(!runtime.isListening());
        QCOMPARE(runtime.clientCount(), 0);
        QCOMPARE(runtime.serverPort(), quint16(0));
        QTRY_COMPARE(first.state(), QAbstractSocket::UnconnectedState);
        QTRY_COMPARE(second.state(), QAbstractSocket::UnconnectedState);
        QVERIFY(!runtime.start(path, false, QHostAddress::LocalHost, 0));
    }
    QCOMPARE(QSqlDatabase::connectionNames(), before);
    // Independent new runtime uses a new worker-owned connection, same data.
    ServerRuntime restarted;
    QSignalSpy ready(&restarted, &ServerRuntime::listening);
    QSignalSpy stopped(&restarted, &ServerRuntime::stopped);
    QVERIFY(restarted.start(path, false, QHostAddress::LocalHost, 0));
    QTRY_COMPARE(ready.size(), 1);
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, restarted.serverPort());
    QVERIFY(socket.waitForConnected(1000));
    const auto login = exchange(socket, "USER_LOGIN", {{"phone", "13800138000"}});
    QVERIFY(login.success);
    const auto profile = exchange(socket, "GET_USER_INFO", {});
    QCOMPARE(profile.data["user"].toObject()["nickname"].toString(), QString("线程测试"));
    restarted.stop();
    QTRY_COMPARE(stopped.size(), 1);
    QCOMPARE(QSqlDatabase::connectionNames(), before);
}

void ServerRuntimeTest::databaseWaitDoesNotBlockMainEventLoop()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ServerRuntime runtime;
    QSignalSpy ready(&runtime, &ServerRuntime::listening);
    QSignalSpy stopped(&runtime, &ServerRuntime::stopped);
    const QString path = directory.filePath("locked.sqlite3");
    QVERIFY(runtime.start(path, false, QHostAddress::LocalHost, 0));
    QTRY_COMPARE(ready.size(), 1);
    // This is a separate connection created and used only by the test thread.
    charging::server::DatabaseConnection blocker;
    QVERIFY(blocker.open(path, false));
    QSqlQuery query(blocker.database());
    QVERIFY(query.exec("BEGIN IMMEDIATE"));
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, runtime.serverPort());
    QVERIFY(socket.waitForConnected(1000));
    socket.write(requestFrame("USER_LOGIN", {{"phone", "13800138001"}}, "blocked"));
    QVERIFY(socket.waitForBytesWritten(1000));
    int ticks = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, this, [&]() { ++ticks; });
    heartbeat.start();
    bool released = false;
    QTimer::singleShot(250, this, [&]() { released = query.exec("ROLLBACK"); });
    QTRY_VERIFY(released);
    QVERIFY(ticks >= 3);
    const auto replies = receive(socket);
    QCOMPARE(replies.size(), 1);
    QVERIFY(replies[0].success);
    runtime.stop();
    QTRY_COMPARE(stopped.size(), 1);
}

void ServerRuntimeTest::startupFailuresReleaseConnections()
{
    const auto before = QSqlDatabase::connectionNames();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTcpServer occupied;
    QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
    for (int scenario = 0; scenario < 2; ++scenario) {
        ServerRuntime runtime;
        QSignalSpy failed(&runtime, &ServerRuntime::startupFailed);
        QSignalSpy stopped(&runtime, &ServerRuntime::stopped);
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        const QString path = scenario == 0 ? directory.path() : directory.filePath("port.sqlite3");
        QVERIFY(runtime.start(path, false, QHostAddress::LocalHost,
                              scenario == 0 ? 0 : occupied.serverPort()));
        QTRY_COMPARE(stopped.size(), 1);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(ready.size(), 0);
        QVERIFY(!failed.first().first().toString().contains(directory.path()));
        QCOMPARE(QSqlDatabase::connectionNames(), before);
    }
}

void ServerRuntimeTest::stopDuringStartupAndDestructorFallback()
{
    const auto before = QSqlDatabase::connectionNames();
    for (int i = 0; i < 12; ++i) {
        ServerRuntime runtime;
        QSignalSpy ready(&runtime, &ServerRuntime::listening);
        QSignalSpy stopped(&runtime, &ServerRuntime::stopped);
        QVERIFY(runtime.start(":memory:", true, QHostAddress::LocalHost, 0));
        runtime.stop();
        QTRY_COMPARE(stopped.size(), 1);
        QCOMPARE(ready.size(), 0);
    }
    for (int i = 0; i < 12; ++i) {
        ServerRuntime runtime;
        QVERIFY(runtime.start(":memory:", false, QHostAddress::LocalHost, 0));
        // Immediate destruction must join even if run() hasn't initialized yet.
    }
    QCOMPARE(QSqlDatabase::connectionNames(), before);
}

QTEST_GUILESS_MAIN(ServerRuntimeTest)
#include "tst_server_runtime.moc"
