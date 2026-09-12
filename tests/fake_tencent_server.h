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
                // readyRead 可能分片到达：请求字节攒在 socket 动态属性里，
                // 见首行换行符才处理；chargingAnswered 幂等闸保证迟到分片
                // 不再二次解析、一条连接只回一次包。
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
                    // 攥 socket 用 QPointer：等待期间客户端超时断连、disconnected
                    // 分支已 deleteLater 时，releasePending 判空自动跳过，不悬垂。
                    if (holdRequests_) {
                        pending_.append(QPointer<QTcpSocket>(socket));
                        return;
                    }
                    writeResponse(socket, status_, body_, retryAfter_);
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

    // 预置应答脚本：此后每条请求都回这份 status/body（403/5xx 失败用例经此
    // 注入；同一用例内可多次 set 换剧本，只影响 set 之后到达的请求）。
    void setResponse(int status, const QByteArray& body)
    {
        status_ = status;
        body_ = body;
    }
    void setJsonResponse(const QByteArray& json) { setResponse(200, json); }
    void setRetryAfter(const QByteArray& value) { retryAfter_ = value; }
    // 扣住所有请求不回包：配合 setRequestTimeoutForTesting 驱动超时用例，
    // 或稍后 releasePending 精确控制“模拟数据先渲染、真实响应后到”的时序。
    void setHoldRequests(bool hold) { holdRequests_ = hold; }
    // 一次性放行全部被扣请求：先整体换出挂起队列（重复 release 不会对同一
    // 连接二次回包），再逐个统一回 200 + json（放行包不受 status_/body_ 影响）。
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
    static void writeResponse(QTcpSocket* socket, int status, const QByteArray& body,
                              const QByteArray& retryAfter = {})
    {
        const char* reason = status == 200 ? "OK" : (status == 403 ? "Forbidden" : "Error");
        QByteArray head = QByteArray("HTTP/1.1 ") + QByteArray::number(status) + ' '
            + reason + "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n";
        if (!retryAfter.isEmpty()) head += "Retry-After: " + retryAfter + "\r\n";
        head += "\r\n";
        // 回包即主动断连：与头部 Connection: close 呼应，客户端必快收到
        // finished，简化时序（不做 keep-alive）。
        socket->write(head + body);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    int status_ = 200;
    QByteArray body_;
    QByteArray retryAfter_;
    // “扣住-放行”编排状态：holdRequests_ 为电平开关，pending_ 存被攥住的
    // 连接（QPointer 容忍放行前已断连的 socket）。
    bool holdRequests_ = false;
    // 断言日志：连接数、最后一次/全量 target 序列，测试经同名 getter 事后
    // 核对“打了几次、打的什么路径”（顺序口径见 getter 处注释）。
    int connectionCount_ = 0;
    QString lastRequestTarget_;
    QStringList requestTargets_;
    QList<QPointer<QTcpSocket>> pending_;
};

} // namespace charging::testing
