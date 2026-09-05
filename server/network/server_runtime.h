#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace charging::server {

class ServerThread;

// GUI-thread facade. No socket, service, or SQL handle crosses this boundary.
// One-shot lifecycle: create a new runtime to restart a stopped server.
class ServerRuntime final : public QObject
{
    Q_OBJECT
public:
    explicit ServerRuntime(QObject* parent = nullptr);
    ~ServerRuntime() override;

    bool start(const QString& databasePath, bool demoSeed,
               const QHostAddress& address, quint16 port);
    void stop(); // asynchronous; stopped() means all worker resources are gone
    bool isListening() const;
    quint16 serverPort() const;
    int clientCount() const;
    void submitAdminRequest(const QString& requestId, const QString& action,
                            const QJsonObject& data, const QString& sessionToken,
                            qint64 deadlineMs);

signals:
    void listening(quint16 port);
    void clientCountChanged(int count);
    void startupFailed(const QString& message);
    void stopped();
    void adminResponse(const QString& requestId, const QJsonObject& response);

private:
    enum class State { Idle, Starting, Running, Stopping, Stopped };
    ServerThread* thread_ = nullptr;
    State state_ = State::Idle;
    quint16 port_ = 0;
    int clients_ = 0;
};

} // namespace charging::server
