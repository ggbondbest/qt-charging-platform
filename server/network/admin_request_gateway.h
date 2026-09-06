#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QTimer;
namespace charging::server {
class ServerRuntime;

// GUI-thread facade. Pass a page as owner and a stable key for each refresh
// stream; destroyed owners and superseded responses are silently discarded.
class AdminRequestGateway final : public QObject
{
    Q_OBJECT
public:
    explicit AdminRequestGateway(ServerRuntime* runtime, QObject* parent = nullptr);
    ~AdminRequestGateway() override;
    QString request(const QString& action, const QJsonObject& data, QObject* owner,
                    const QString& key = {}, int timeoutMs = 10000);
    void logout();
    bool isAuthenticated() const
    {
        return !token_.isEmpty();
    }
signals:
    void finished(const QString& requestId, const QJsonObject& response);
    void authenticationChanged(bool authenticated);

private:
    struct Pending
    {
        QPointer<QObject> owner;
        QPointer<QTimer> timer;
        QString action;
        QString key;
        quint64 generation;
        QMetaObject::Connection ownerConnection;
    };
    void receive(const QString& id, QJsonObject response);
    void forget(const QString& id);
    void invalidate();
    void revoke(const QString& token);
    QPointer<ServerRuntime> runtime_;
    QHash<QString, Pending> pending_;
    QString token_;
    QString requestPrefix_;
    quint64 generation_ = 0;
};
} // namespace charging::server
