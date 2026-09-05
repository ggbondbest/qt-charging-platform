#include "server_runtime.h"

#include "billing_service.h"
#include "charging_repository.h"
#include "charging_server.h"
#include "charging_service.h"
#include "database_connection.h"
#include "order_repository.h"
#include "order_service.h"
#include "request_dispatcher.h"
#include "user_api_repository.h"
#include "user_api_service.h"
#include "user_repository.h"
#include "user_service.h"

#include <QThread>

namespace charging::server {

// The QThread object belongs to the GUI thread; only run() executes in the
// worker. No business slots are placed on this QThread object.
class ServerThread final : public QThread
{
    Q_OBJECT
public:
    using QThread::QThread;
    QString databasePath;
    bool demoSeed = false;
    QHostAddress address;
    quint16 port = 0;

signals:
    void ready(quint16 port);
    void countChanged(int count);
    void failed(const QString& message);

protected:
    void run() override
    {
        try {
            if (isInterruptionRequested()) return;
            // Declaration order is intentional: sockets/sessions die first,
            // then dispatcher/services/repositories, finally the SQL connection.
            DatabaseConnection database;
            QString diagnostic;
            if (!database.open(databasePath, demoSeed, &diagnostic)) {
                emit failed(QStringLiteral("无法初始化服务端数据库"));
                return;
            }
            if (isInterruptionRequested()) return;
            UserRepository users(database.database());
            ChargingRepository charging(database.database());
            OrderRepository orders(database.database());
            UserApiRepository userApi(database.database());
            UserService userService(&users);
            BillingService billing;
            ChargingService chargingService(&charging, &billing);
            OrderService orderService(&orders);
            UserApiService userApiService(&userApi);
            RequestDispatcher dispatcher(&userService, &chargingService, &orderService,
                                         &userApiService);
            ChargingServer server;
            server.setRequestDispatcher(&dispatcher);
            // Signal forwarding only: this direct lambda runs in the worker and
            // never reads or writes GUI-owned state. Facade connections are queued.
            connect(&server, &ChargingServer::clientCountChanged, &server,
                    [this](int count) { emit countChanged(count); });
            if (!server.listen(address, port)) {
                emit failed(QStringLiteral("无法监听服务端地址或端口"));
                return;
            }
            if (isInterruptionRequested()) return;
            emit ready(server.serverPort());
            exec();
        } catch (...) {
            // Do not expose exception text, SQL, credentials or filesystem paths.
            emit failed(QStringLiteral("服务工作线程发生内部错误"));
        }
    }
};

ServerRuntime::ServerRuntime(QObject* parent) : QObject(parent), thread_(new ServerThread(this))
{
    thread_->setObjectName(QStringLiteral("charging-service-worker"));
    connect(thread_, &ServerThread::ready, this, [this](quint16 port) {
        if (state_ != State::Starting) return;
        state_ = State::Running;
        port_ = port;
        emit listening(port);
    }, Qt::QueuedConnection);
    connect(thread_, &ServerThread::countChanged, this, [this](int count) {
        if (state_ != State::Running) return;
        clients_ = count;
        emit clientCountChanged(count);
    }, Qt::QueuedConnection);
    connect(thread_, &ServerThread::failed, this, [this](const QString& message) {
        if (state_ == State::Starting || state_ == State::Running) {
            state_ = State::Stopping;
            emit startupFailed(message);
        }
    }, Qt::QueuedConnection);
    connect(thread_, &QThread::finished, this, [this]() {
        state_ = State::Stopped;
        port_ = 0;
        clients_ = 0;
        emit clientCountChanged(0);
        emit stopped();
    }, Qt::QueuedConnection);
}

ServerRuntime::~ServerRuntime()
{
    // Fallback for owners destroyed without observing stopped(). Normal UI exit
    // is asynchronous. Never terminate a thread during a SQLite transaction.
    thread_->requestInterruption();
    thread_->quit();
    thread_->wait();
}

bool ServerRuntime::start(const QString& databasePath, bool demoSeed,
                          const QHostAddress& address, quint16 port)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ != State::Idle) return false;
    state_ = State::Starting;
    thread_->databasePath = databasePath;
    thread_->demoSeed = demoSeed;
    thread_->address = address;
    thread_->port = port;
    thread_->start();
    return true;
}

void ServerRuntime::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ == State::Stopping || state_ == State::Stopped) return;
    if (state_ == State::Idle) {
        state_ = State::Stopped;
        emit stopped();
        return;
    }
    state_ = State::Stopping;
    port_ = 0;
    thread_->requestInterruption();
    thread_->quit(); // thread-safe; current request completes before stack cleanup
}

bool ServerRuntime::isListening() const { return state_ == State::Running; }
quint16 ServerRuntime::serverPort() const { return port_; }
int ServerRuntime::clientCount() const { return clients_; }

} // namespace charging::server

#include "server_runtime.moc"
