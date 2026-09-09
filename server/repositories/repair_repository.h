#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QSqlDatabase>
#include <stdexcept>

namespace charging::server {

struct RepairFailure : std::runtime_error
{
    explicit RepairFailure(const char* code) : std::runtime_error(code) {}
};

// Called only by the authenticated service worker. Every mutation owns one
// transaction containing the report, timeline, charger, notification and audit.
class RepairRepository final
{
public:
    explicit RepairRepository(const QSqlDatabase& database);
    QJsonObject handle(const QString& type, const QJsonObject& parameters, qint64 userId,
                       const QDateTime& now) const;
    QJsonObject adminHandle(const QString& action, const QJsonObject& parameters, qint64 adminId,
                            const QDateTime& now) const;

private:
    QSqlDatabase database_;
};

} // namespace charging::server
