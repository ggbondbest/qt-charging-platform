#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QSqlDatabase>

namespace charging::server {

class QueueRepository final
{
public:
    static constexpr qint64 kCallLifetimeSeconds = 60;
    explicit QueueRepository(const QSqlDatabase& database);
    QJsonObject execute(const QString& type, const QJsonObject& data, qint64 userId,
                        const QDateTime& now) const;
    QJsonObject adminRead(const QJsonObject& data, const QDateTime& now) const;
    // Persistent, transaction-protected FIFO; safe to run repeatedly or after restart.
    bool tick(const QDateTime& now, QString* diagnostic = nullptr) const;

private:
    QSqlDatabase database_;
};
} // namespace charging::server
