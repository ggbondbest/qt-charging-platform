#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

namespace charging::testing {

// 进程内假腾讯 WebService（成员 2 地图接入测试夹具）：
// 监听 127.0.0.1 随机端口，可配置 HTTP 状态与 JSON 响应体；支持“扣住不回包”
// （驱动超时与确定性时序：页面先渲染模拟数据，测试再放行真实响应）。
// 单测永不触真实网络（CI 无 key、无外网也能全绿）。
class FakeTencentServer final
{
public:
    bool start()
    {
        if (!server_.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                ++connectionCount_;
                auto buffer = QByteArray();
                socket->setProperty("chargingRequest", QVariant::fromValue(buffer));
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray request =
                        socket->property("chargingRequest").toByteArray();
                    request.append(socket->readAll());
                    socket->setProperty("chargingRequest", request);
                    const int newline = request.indexOf('\n');
                    if (newline < 0 || socket->property("chargingAnswered").toBool()) {
                        return;
                    }
                    socket->setProperty("chargingAnswered", true);
                    // 请求行 "GET <target> HTTP/1.1" → 记录完整 target（path+query）。
                    const QByteArray line = request.left(newline).trimmed();
                    lastRequestTarget_ = QString::fromLatin1(line.split(' ').value(1));
                    requestTargets_ << lastRequestTarget_;
                    if (holdRequests_) {
                        pending_.append(QPointer<QTcpSocket>(socket));
                        return;
                    }
                    writeResponse(socket, status_, body_);
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 &QObject::deleteLater);
            }
        });
        return true;
    }

    void stop() { server_.close(); }

    QString endpointBase() const // 传给 setEndpointBaseForTesting 的 base
    {
        return QStringLiteral("http://127.0.0.1:%1/ws").arg(server_.serverPort());
    }

    int connectionCount() const { return connectionCount_; }
    QString lastRequestTarget() const { return lastRequestTarget_; }
    // 并发多请求时（路线+逆地理）last 不保证顺序，用全量列表断言。
    QStringList requestTargets() const { return requestTargets_; }

    void setResponse(int status, const QByteArray& body)
    {
        status_ = status;
        body_ = body;
    }
    void setJsonResponse(const QByteArray& json) { setResponse(200, json); }
    // 扣住所有请求不回包：配合 setRequestTimeoutForTesting 驱动超时用例，
    // 或稍后 releasePending 精确控制“模拟数据先渲染、真实响应后到”的时序。
    void setHoldRequests(bool hold) { holdRequests_ = hold; }
    void releasePending(const QByteArray& json)
    {
        const QList<QPointer<QTcpSocket>> sockets = std::exchange(pending_, {});
        for (const auto& socket : sockets) {
            if (!socket.isNull()) {
                writeResponse(socket.data(), 200, json);
            }
        }
    }

    // 借临时端口后立即关闭：返回一个确定无人监听的端口（拒连用例）。
    static quint16 closedPort()
    {
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        return probe.serverPort();
    }

private:
    static void writeResponse(QTcpSocket* socket, int status, const QByteArray& body)
    {
        const char* reason = status == 200 ? "OK" : (status == 403 ? "Forbidden" : "Error");
        QByteArray head = QByteArray("HTTP/1.1 ") + QByteArray::number(status) + ' '
            + reason + "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(head + body);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    int status_ = 200;
    QByteArray body_;
    bool holdRequests_ = false;
    int connectionCount_ = 0;
    QString lastRequestTarget_;
    QStringList requestTargets_;
    QList<QPointer<QTcpSocket>> pending_;
};

} // namespace charging::testing
