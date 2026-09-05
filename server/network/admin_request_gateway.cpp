#include "admin_request_gateway.h"
#include "admin_service.h"
#include "server_runtime.h"

#include <QDateTime>
#include <QThread>
#include <QTimer>
#include <QUuid>

namespace charging::server {
AdminRequestGateway::AdminRequestGateway(ServerRuntime* runtime, QObject* parent)
    : QObject(parent), runtime_(runtime)
{
    requestPrefix_ = QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char(':');
    connect(runtime, &ServerRuntime::adminResponse, this, &AdminRequestGateway::receive);
    const auto stopped = [this] {
        token_.clear();
        ++generation_;
        const auto ids = pending_.keys();
        for (const auto& id : ids) {
            const auto owner = pending_.value(id).owner;
            forget(id);
            if (owner)
                emit finished(id, AdminService::failure(QStringLiteral("UNAVAILABLE")));
        }
        emit authenticationChanged(false);
    };
    connect(runtime, &ServerRuntime::stopped, this, stopped);
    connect(runtime, &QObject::destroyed, this, stopped);
    auto* sessionCheck = new QTimer(this);
    sessionCheck->setInterval(30000);
    connect(sessionCheck, &QTimer::timeout, this, [this] {
        if (!token_.isEmpty())
            request(QStringLiteral("auth.check"), {}, this, QStringLiteral("session-check"));
    });
    sessionCheck->start();
}
AdminRequestGateway::~AdminRequestGateway()
{
    // FIFO with this gateway's requests: also removes a login that is still in
    // flight and whose response cannot be received after our destruction.
    if (runtime_)
        runtime_->submitAdminRequest(requestPrefix_ + QStringLiteral("close"),
                                     QStringLiteral("auth.close"), {}, {},
                                     QDateTime::currentMSecsSinceEpoch() + 10000);
}
void AdminRequestGateway::revoke(const QString& token)
{
    if (runtime_ && !token.isEmpty())
        runtime_->submitAdminRequest(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                     QStringLiteral("auth.logout"), {}, token,
                                     QDateTime::currentMSecsSinceEpoch() + 10000);
}
void AdminRequestGateway::forget(const QString& id)
{
    if (!pending_.contains(id))
        return;
    const auto entry = pending_.take(id);
    disconnect(entry.ownerConnection);
    if (entry.timer) {
        entry.timer->stop();
        entry.timer->deleteLater();
    }
}
void AdminRequestGateway::invalidate()
{
    revoke(token_);
    token_.clear();
    ++generation_;
    const auto ids = pending_.keys();
    for (const auto& id : ids)
        forget(id);
    emit authenticationChanged(false);
}
void AdminRequestGateway::logout()
{
    invalidate();
}

QString AdminRequestGateway::request(const QString& action, const QJsonObject& data, QObject* owner,
                                     const QString& key, int timeoutMs)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!owner || owner->thread() != thread())
        return {};
    if (action == QStringLiteral("auth.login"))
        invalidate();
    if (action == QStringLiteral("auth.logout")) {
        logout();
        return {};
    }
    // One current result per page/refresh key, and a bounded pending queue.
    const auto ids = pending_.keys();
    for (const auto& id : ids) {
        const auto& entry = pending_[id];
        if (!key.isEmpty() && entry.owner == owner && entry.key == key)
            forget(id);
    }
    if (pending_.size() >= 128)
        return {};
    const QString id = requestPrefix_ + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    const auto connection = connect(owner, &QObject::destroyed, this, [this, id] { forget(id); });
    pending_.insert(id, {owner, timer, action, key, generation_, connection});
    connect(timer, &QTimer::timeout, this, [this, id] {
        if (!pending_.contains(id))
            return;
        const auto owner = pending_.value(id).owner;
        forget(id);
        if (owner)
            emit finished(id, AdminService::failure(QStringLiteral("TIMEOUT")));
    });
    const int duration = qBound(1, timeoutMs, 60000);
    timer->start(duration);
    if (runtime_)
        runtime_->submitAdminRequest(id, action, data, token_,
                                     QDateTime::currentMSecsSinceEpoch() + duration);
    else
        QTimer::singleShot(0, this, [this, id] {
            receive(id, AdminService::failure(QStringLiteral("UNAVAILABLE")));
        });
    return id;
}

void AdminRequestGateway::receive(const QString& id, QJsonObject response)
{
    if (!id.startsWith(requestPrefix_))
        return;
    auto data = response.value(QStringLiteral("data")).toObject();
    const QString newToken = data.take(QStringLiteral("sessionToken")).toString();
    // Timed-out, abandoned or superseded login responses must revoke the
    // capability they created; never resurrect a logged-out UI session.
    if (!pending_.contains(id)) {
        revoke(newToken);
        return;
    }
    const auto entry = pending_.value(id);
    forget(id);
    if (!entry.owner || entry.generation != generation_) {
        revoke(newToken);
        return;
    }
    if (entry.action == QStringLiteral("auth.login") &&
        response.value(QStringLiteral("success")).toBool()) {
        token_ = newToken;
        response.insert(QStringLiteral("data"), data);
        emit authenticationChanged(true);
    }
    if (response.value(QStringLiteral("error"))
            .toObject()
            .value(QStringLiteral("code"))
            .toString() == QStringLiteral("UNAUTHORIZED"))
        invalidate();
    emit finished(id, response);
}
} // namespace charging::server
